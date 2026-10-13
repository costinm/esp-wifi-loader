#!/usr/bin/env python3
"""Discover esp-wifi-loader devices and upload a signed package.

  upload.py discover [--iface wlan0] [--seconds 10]
  upload.py send PACKAGE --host fe80::1%wlan0 [--port 5227] [--dry]
  upload.py send PACKAGE --host 192.168.4.1 --port 5228 [--adb SERIAL]   # an application's upload port (IPv4 too)
  upload.py reboot --host fe80::1%wlan0

An application that links the loader's upload code (esp-xip-modules `main`, port 5228) takes the same bytes, so this one
tool also sends modules and applications to it: any host address, `--port`, and `--adb SERIAL` to go through an Android
phone that is attached with adb and joined to the device's network (its `nc` carries the bytes) when the computer has
no Wi-Fi of its own.

A Linux host runs the flasher access point (hostapd, WPA2, SSID DIRECT-xy-flash) and shares no
internet with it. See docs/protocol.md.
"""
import argparse
import os
import socket
import struct
import subprocess
import sys
import time

import wl_format as wf

GROUP = "ff02::5227"
PORT = 5227


def parse_announce(line: str) -> dict:
    parts = line.split()
    if not parts or parts[0] != "esp-wifi-loader":
        raise ValueError("not an announce")
    return dict(p.split("=", 1) for p in parts[1:] if "=" in p)


def discover(iface, seconds, port=PORT):
    idx = socket.if_nametoindex(iface) if iface else 0
    s = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("::", port))
    s.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_JOIN_GROUP,
                 socket.inet_pton(socket.AF_INET6, GROUP) + struct.pack("@I", idx))
    s.settimeout(1)
    seen = {}
    end = time.time() + seconds
    while time.time() < end:
        try:
            data, addr = s.recvfrom(1500)
        except socket.timeout:
            continue
        try:
            info = parse_announce(data.decode())
        except (ValueError, UnicodeDecodeError):
            continue
        host = addr[0] if "%" in addr[0] or not addr[3] else f"{addr[0]}%{addr[3]}"
        if host not in seen:
            seen[host] = info
            print(host, info, flush=True)
    return seen


def _read_status(sock):
    buf = b""
    while True:
        try:
            chunk = sock.recv(256)
        except socket.timeout:
            return "ERR no-reply"
        if not chunk:
            break
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            line = line.decode()
            print(line, flush=True)
            if line.startswith(("OK", "ERR")):
                return line
    return buf.decode() or "ERR no-reply"


ADB = os.environ.get("ADB", "adb")


def adb_nc(serial, host, port, data: bytes, udp=False, wait=3, timeout=60):
    """Send `data` through the phone's netcat and return what the device answered. TCP ends when the device closes
    (it does after its verdict); UDP has no end, so the phone's `timeout` stops it after `wait` seconds."""
    nc = f"nc {'-u ' if udp else ''}-w {wait} {host} {port}"
    cmd = [ADB, "-s", serial, "shell", f"timeout {wait + 1} {nc}" if udp else nc]
    try:
        r = subprocess.run(cmd, input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
        return r.stdout
    except subprocess.TimeoutExpired as e:
        return e.stdout or b""


def send_adb(serial, host, port, body: bytes, dry=False, reboot=False):
    out = adb_nc(serial, host, port, wf.transport_header(dry=dry, reboot=reboot) + body, wait=20, timeout=120)
    lines = [l for l in out.decode(errors="replace").replace("\r", "").split("\n") if l]
    for l in lines:
        if not l.startswith("P "):
            print(l, flush=True)
    return next((l for l in reversed(lines) if l.startswith(("OK", "ERR"))), "ERR no-reply")


def send(host, port, body: bytes, dry=False, reboot=False):
    """Any address family: `fe80::1%wlan0` (the loader), `192.168.4.1` or a name (an application)."""
    sock = socket.create_connection((host, port), timeout=30)
    sock.settimeout(30)
    sock.sendall(wf.transport_header(dry=dry, reboot=reboot) + body)
    sock.shutdown(socket.SHUT_WR)  # the client FIN ends the upload
    status = _read_status(sock)
    sock.close()
    return status


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("discover"); d.add_argument("--iface"); d.add_argument("--seconds", type=int, default=10)
    s = sub.add_parser("send"); s.add_argument("package"); s.add_argument("--host", required=True)
    s.add_argument("--port", type=int, default=PORT); s.add_argument("--dry", action="store_true")
    s.add_argument("--adb", metavar="SERIAL", help="go through this phone's netcat")
    r = sub.add_parser("reboot"); r.add_argument("--host", required=True); r.add_argument("--port", type=int, default=PORT)
    a = ap.parse_args()
    if a.cmd == "discover":
        discover(a.iface, a.seconds)
        return 0
    if a.cmd == "send":
        with open(a.package, "rb") as f:
            status = (send_adb(a.adb, a.host, a.port, f.read(), dry=a.dry) if a.adb
                      else send(a.host, a.port, f.read(), dry=a.dry))
    else:
        status = send(a.host, a.port, b"", reboot=True)
    return 0 if status.startswith("OK") else 1


if __name__ == "__main__":
    sys.exit(main())

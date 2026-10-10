#!/usr/bin/env python3
"""Discover esp-wifi-loader devices and upload a signed package.

  upload.py discover [--iface wlan0] [--seconds 10]
  upload.py send PACKAGE --host fe80::1%wlan0 [--port 5227] [--dry]
  upload.py reboot --host fe80::1%wlan0

A Linux host runs the ESP-FLASH access point (hostapd, open) and shares no
internet with it. See docs/protocol.md.
"""
import argparse
import socket
import struct
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
        chunk = sock.recv(256)
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


def send(host, port, body: bytes, dry=False, reboot=False):
    infos = socket.getaddrinfo(host, port, socket.AF_INET6, socket.SOCK_STREAM)
    sock = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
    sock.settimeout(30)
    sock.connect(infos[0][4])
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
    r = sub.add_parser("reboot"); r.add_argument("--host", required=True); r.add_argument("--port", type=int, default=PORT)
    a = ap.parse_args()
    if a.cmd == "discover":
        discover(a.iface, a.seconds)
        return 0
    if a.cmd == "send":
        with open(a.package, "rb") as f:
            status = send(a.host, a.port, f.read(), dry=a.dry)
    else:
        status = send(a.host, a.port, b"", reboot=True)
    return 0 if status.startswith("OK") else 1


if __name__ == "__main__":
    sys.exit(main())

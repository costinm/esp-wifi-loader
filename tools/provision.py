#!/usr/bin/env python3
"""Put an application ("main") on a board through esp-wifi-loader and an Android phone, and check each step.

  provision.py check   [BOARD]   read-only: board identity, layout, pinned key, main; phone and packages
  provision.py backup  [BOARD]   read the whole flash into target/backups/
  provision.py prepare [BOARD] --yes   clear the pinned key and blank main, so the board boots into the loader
  provision.py push    [BOARD] --yes   copy the signed main to the phone's app directory
  provision.py flash   [BOARD] --yes   the phone hosts the flash network (Wi-Fi Direct) and installs main
  provision.py verify  [BOARD]   reset the board and read its console: did main start
  provision.py join    [BOARD]   the phone joins the board's access point, to carry uploads and tests (--yes)
  provision.py all     [BOARD] --yes   check, backup, prepare, push, flash, verify, join

Run it from the application's checkout: boards and the phone come from tests/device.toml there (--config; the current
lab; copy and edit for another one), the signed package is target/rel/<chip>.wlpkg (--rel) and backups go to
target/backups (--backups). Board entries: name, chip, mac, port (serial), phone (adb serial), kind (package name on
the phone, default "app"), optional baud, ap_prefix / ap_password (the application's own access point, for `join`).
Nothing is written without --yes, and never to a board whose MAC differs from the configured one: the port is only a way
to reach it. Needs ESP-IDF's python (for esptool) and adb.
"""
import argparse
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import tomllib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
REL_DIR = os.path.join("target", "rel")
BACKUP_DIR = os.path.join("target", "backups")

# The layout esp-wifi-loader's Stage2 writes (docs/boot.md there). The state log is the end of the loader partition.
LAYOUT = {"loader": (0x10000, 0xE0000), "main": (0xF0000, 0x2D0000)}
STATE_OFFSET, STATE_SIZE = 0xEE000, 0x2000
APP = "com.github.costinm.espwifiloader"
APP_DIR = f"/sdcard/Android/data/{APP}/files/packages"
REC = {1: "pin", 2: "main_min_version", 3: "main_header", 4: "qio", 5: "loader_min_version", 6: "stage2_min_version", 7: "module_min_version"}


# ---- decoding what is in the flash (pure functions, tested against real dumps) ----
def partition_table(raw: bytes):
    """[(name, type, subtype, offset, size)] from a 0x8000 table dump."""
    out = []
    for i in range(0, len(raw), 32):
        e = raw[i:i + 32]
        if e[:2] != b"\xaa\x50":
            break
        typ, sub, off, size = struct.unpack("<BBII", e[2:12])
        out.append((e[12:28].rstrip(b"\0").decode(), typ, sub, off, size))
    return out


def _crc16(crc, data):
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def state_log(raw: bytes):
    """The latest value of each record of the loader's state log (two 4 KiB sectors), or None when blank."""
    best = None
    for s in (0, 1):
        sec = raw[s * 4096:(s + 1) * 4096]
        if len(sec) < 16 or sec[:4] != b"WLS1" or struct.unpack("<I", sec[8:12])[0] != 0xA5A5A5A5:
            continue
        seq = struct.unpack("<I", sec[4:8])[0]
        if best is None or ((seq - best[0]) & 0xFFFFFFFF) < 0x80000000 and seq != best[0]:
            best = (seq, sec)
    if best is None:
        return None
    view, off, sec = {}, 16, best[1]
    while off + 4 <= len(sec):
        word = struct.unpack("<I", sec[off:off + 4])[0]
        if word == 0xFFFFFFFF:
            break
        typ, ln, crc = word & 0xFF, (word >> 8) & 0xFF, word >> 16
        data = sec[off + 4:off + 4 + ln]
        if _crc16(_crc16(0xFFFF, bytes([typ, ln])), data) != crc:
            break
        name = REC.get(typ, f"type{typ}")
        view[name] = data
        off += 4 + ((ln + 3) & ~3)
    return view


def describe_state(view):
    if view is None:
        return "blank (no key pinned)"
    parts = []
    if "pin" in view:
        parts.append("key pinned: " + hashlib.sha256(view["pin"]).hexdigest()[:8])
    if "main_min_version" in view:
        parts.append("main version >= %d" % struct.unpack("<I", view["main_min_version"])[0])
    if "qio" in view:
        parts.append({1: "qio ok", 2: "dio only"}.get(view["qio"][0], "qio ?"))
    return ", ".join(parts) or "active, no records"


def boot_target(nvs: bytes):
    """The value of NVS stg2:boot_target (1 = always main, 2 = always the loader) if the partition holds one, else 0.
    A plain scan for the key, not a full NVS parse: a deleted entry could still be found."""
    i = nvs.find(b"boot_target\0")
    if i < 0 or nvs.find(b"stg2\0") < 0:
        return 0
    return struct.unpack("<I", nvs[i + 16:i + 20])[0]


def image_head(raw: bytes):
    if len(raw) < 16 or raw[0] != 0xE9:
        return "no valid image (first byte %02x)" % (raw[0] if raw else 0)
    chip = struct.unpack("<H", raw[12:14])[0]
    return "valid image (chip id %d, %d segments)" % (chip, raw[1])


# ---- the tools ----
class Lab:
    def __init__(self, board, config, run=subprocess.run):
        self.b, self.cfg, self._run = board, config, run
        self.name = board["name"]
        self.port, self.phone = board["port"], board.get("phone")
        self.adb = shutil.which("adb") or config.get("adb", "adb")

    def backups(self):
        return self.cfg.get("backups", BACKUP_DIR)

    def say(self, text):
        print(f"[{self.name}] {text}", flush=True)

    def sh(self, cmd, check=True, **kw):
        r = self._run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, **kw)
        if check and r.returncode:
            raise SystemExit(f"failed: {' '.join(map(str, cmd))}\n{r.stdout}")
        return r.stdout

    def esptool(self, *args, **kw):
        return self.sh([sys.executable, "-m", "esptool", "--port", self.port, *args], **kw)

    def adbs(self, *args, **kw):
        return self.sh([self.adb, "-s", self.phone, *args], **kw)

    # -- board --
    def identity(self):
        out = self.esptool("chip-id")
        mac = re.search(r"^MAC:\s+([0-9a-f:]+)", out, re.M | re.I)
        chip = re.search(r"Chip type:\s+(\S+)", out)
        found = mac.group(1).lower() if mac else None
        want = self.b["mac"].lower().replace("-", ":")
        # USB-Serial/JTAG chips report an EUI-64 (ff:fe in the middle); the board's own MAC is what is configured.
        if found and len(found.split(":")) == 8 and found.split(":")[3:5] == ["ff", "fe"]:
            parts = found.split(":")
            found = ":".join(parts[:3] + parts[5:])
        if found != want:
            raise SystemExit(f"{self.port} is {found}, not {want} ({self.name}): refusing to touch it")
        return chip.group(1) if chip else "?"

    def read(self, offset, size, quiet=True):
        path = os.path.join(os.environ.get("TMPDIR", "/tmp"), f"em-read-{os.getpid()}.bin")
        self.esptool("--baud", str(self.b.get("baud", 921600)), "read-flash", hex(offset), hex(size), path)
        data = open(path, "rb").read()
        os.unlink(path)
        return data

    def check(self):
        chip = self.identity()
        self.say(f"{chip}, MAC {self.b['mac']} on {self.port}")
        table = partition_table(self.read(0x8000, 0xC00))
        got = {n: (o, s) for n, _, _, o, s in table}
        for n, want in LAYOUT.items():
            ok = got.get(n) == want
            self.say(f"partition {n}: {'0x%x/0x%x' % got[n] if n in got else 'missing'}" + ("" if ok else f"  (expected 0x{want[0]:x}/0x{want[1]:x})"))
        if "data" in got:
            self.say("partition data: 0x%x/0x%x (module slots)" % got["data"])
        self.say("state log: " + describe_state(state_log(self.read(STATE_OFFSET, STATE_SIZE))))
        bt = boot_target(self.read(0x9000, 0x6000))
        if bt:
            self.say(f"NVS stg2:boot_target = {bt}: Stage2 obeys it before the reset counters, so five quick resets will NOT "
                     "start the loader; `prepare` erases the NVS")
        self.say("main: " + image_head(self.read(LAYOUT["main"][0], 16)))
        self.say("loader: " + image_head(self.read(LAYOUT["loader"][0], 16)))
        self.check_phone()

    # -- phone --
    def check_phone(self):
        if not self.phone:
            self.say("no phone configured")
            return
        devices = self.sh([self.adb, "devices"])
        if not re.search(rf"^{re.escape(self.phone)}\s+device", devices, re.M):
            raise SystemExit(f"phone {self.phone} is not attached (adb devices)")
        model = self.adbs("shell", "getprop", "ro.product.model").strip()
        installed = APP in self.adbs("shell", "pm", "list", "packages")
        files = self.adbs("shell", "ls", APP_DIR, check=False)
        power = re.search(r"powered: true", self.adbs("shell", "dumpsys", "battery"))
        self.say(f"phone {self.phone} ({model}): flasher app {'installed' if installed else 'MISSING'}, "
                 f"{'charging' if power else 'on battery'}, packages: {', '.join(sorted(re.findall(r'[\w.-]+\.wlpkg', files)))}")

    # -- the steps --
    def package(self):
        chip = self.b["chip"]
        pkg = os.path.join(self.cfg.get("rel", REL_DIR), f"{chip}.wlpkg")
        if not os.path.exists(pkg):
            raise SystemExit(f"{pkg} is missing: build and sign the application for {chip} first")
        return pkg

    def plan(self, yes, what, cmd):
        shown = " ".join(str(c) for c in cmd)
        if not yes:
            self.say(f"would run (add --yes): {shown}")
            return False
        self.say(what)
        return True

    def backup(self):
        self.identity()
        os.makedirs(self.backups(), exist_ok=True)
        dest = os.path.join(self.backups(), f"{self.name}-{time.strftime('%Y%m%d-%H%M%S')}.bin")
        size = int(re.search(r"(\d+)MB", self.esptool("flash-id")).group(1)) << 20
        self.esptool("--baud", str(self.b.get("baud", 921600)), "read-flash", "0", hex(size), dest)
        self.say(f"backup: {dest} ({size >> 20} MB); restore with: python -m esptool --port {self.port} write-flash 0 {dest}")
        return dest

    def prepare(self, yes):
        cmds = [["erase-region", hex(STATE_OFFSET), hex(STATE_SIZE)], ["erase-region", hex(LAYOUT["main"][0]), "0x10000"]]
        if not self.plan(yes, "clearing the pinned key and the first block of main", cmds[0]):
            self.say("then: " + " ".join(cmds[1]) + " (and the NVS partition, if it holds stg2:boot_target)")
            return
        self.identity()
        backups = self.backups()
        if not (os.path.isdir(backups) and any(f.startswith(self.name + "-") for f in os.listdir(backups))):
            raise SystemExit("no backup of this board in the backup directory: run `backup` first")
        if boot_target(self.read(0x9000, 0x6000)):
            self.say("NVS holds stg2:boot_target (left by another flasher): erasing the NVS partition so Stage2 decides again")
            cmds.append(["erase-region", "0x9000", "0x6000"])
        for c in cmds:
            self.esptool(*c)
        self.say("done; the board restarts into the loader (announce: mode=loader state=blank)")

    def push(self, yes):
        pkg, kind = self.package(), self.b.get("kind", "app")
        name = f"{self.b['chip']}-{kind}.wlpkg"
        if not self.plan(yes, f"pushing {name}", [self.adb, "-s", self.phone, "push", pkg, f"{APP_DIR}/{name}"]):
            return
        self.adbs("push", pkg, f"{APP_DIR}/{name}")
        self.say(f"{name} is on the phone")

    def flash(self, yes, wait=60):
        """The phone hosts the flash network (a Wi-Fi Direct group, DIRECT-ef-flash); the loader scans for it, joins it
        and announces; the phone then sends the package. The group is restarted once if the loader does not show up."""
        kind = self.b.get("kind", "app")
        act = lambda *a: self.adbs("shell", "am", "start", "-n", f"{APP}/.MainActivity", *a)
        if not self.plan(yes, "phone: host the flash network and install main",
                         ["am start ... --es action p2p", f"... --es action flash-all --es bundled {kind}"]):
            return
        self.adbs("shell", "input", "keyevent", "KEYCODE_WAKEUP")
        seen = False
        for attempt in (1, 2):
            act("--es", "action", "stop")
            time.sleep(2)
            self.adbs("logcat", "-c")
            act("--es", "action", "p2p")
            self.say(f"phone hosts DIRECT-ef-flash (attempt {attempt}); the loader joins it by itself, within about 15 s")
            deadline = time.time() + wait
            while time.time() < deadline and not seen:
                time.sleep(3)
                seen = "mode=loader" in self.adbs("logcat", "-d", "-s", "wl-flasher:I")
            if seen:
                break
            self.say("the loader did not show up")
        if not seen:
            raise SystemExit("the phone never saw the loader (is the board in the loader? `check`; its console says `flash network "
                             "found` when it sees the group; phone log: adb logcat -s wl-flasher:I)")
        act("--es", "action", "flash-all", "--es", "bundled", kind)
        deadline = time.time() + wait
        while time.time() < deadline:
            time.sleep(3)
            log = self.adbs("logcat", "-d", "-s", "wl-flasher:I")
            m = re.findall(r"(OK \d+ ms=.*|ERR \d+ [\w-]+)", log)
            if m:
                self.say("loader says: " + m[-1])
                if m[-1].startswith("OK"):
                    return
                raise SystemExit("the loader refused the package: " + m[-1])
        raise SystemExit("no verdict from the loader in time (adb logcat -s wl-flasher:I)")

    def ap_ssid(self):
        """The application's access point is <ap_prefix>-<last two bytes of the SoftAP MAC>; the SoftAP MAC is the base MAC + 1."""
        mac = [int(x, 16) for x in self.b["mac"].split(":")]
        mac[5] = (mac[5] + 1) & 0xFF
        return f"{self.b.get('ap_prefix', 'espmod')}-{mac[4]:02x}{mac[5]:02x}"

    def join(self, yes):
        ssid, pw = self.ap_ssid(), self.b.get("ap_password", "espmodules")
        if not self.plan(yes, f"phone joins {ssid}", [self.adb, "-s", self.phone, "shell", "cmd wifi connect-network", ssid, "wpa2", "***"]):
            return
        self.adbs("shell", "am", "start", "-n", f"{APP}/.MainActivity", "--es", "action", "stop", check=False)
        self.adbs("shell", "cmd", "wifi", "connect-network", ssid, "wpa2", pw, "-r", "non_persistent")
        for _ in range(10):
            time.sleep(2)
            if f'"{ssid}"' in self.adbs("shell", "cmd", "wifi", "status") and "COMPLETED" in self.adbs("shell", "cmd", "wifi", "status"):
                self.say(f"phone is connected to {ssid} ({self.b.get('host', '192.168.4.1')})")
                return
        raise SystemExit(f"the phone did not join {ssid} (is main running? `verify`)")

    def verify(self, seconds=15):
        """Reset the board and read its boot log. The USB-Serial/JTAG port resets the chip when DTR goes low while RTS is
        high (the esptool convention); only the lines' order matters, a plain open does not reset it. main prints
        `ready:` once, at boot, so a reset is how to see it."""
        import reset
        self.say(f"resetting the board and reading the console of {self.port} for {seconds}s")
        c = reset.Port(self.port).open()
        try:
            c.reset()
            marker = self.b.get("ready_marker", "ready:")
            text = c.read(seconds, until=marker)
        finally:
            c.close()
        ok = marker in text
        ap = re.search(r"access point \"([^\"]+)\"", text)
        self.say(("the application is running" if ok else f"no `{marker}` line (is the board booting the application?)")
                 + (f", access point {ap.group(1)}" if ap else ""))
        return ok


def load(config_path, name):
    cfg = tomllib.load(open(config_path, "rb"))
    boards = cfg["board"]
    if name:
        boards = [b for b in boards if b["name"] == name]
    if not boards:
        raise SystemExit(f"no such board in {config_path}")
    return cfg, boards[0]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("step", choices=["check", "backup", "prepare", "push", "flash", "verify", "join", "all"])
    ap.add_argument("board", nargs="?")
    ap.add_argument("--config", default=os.path.join("tests", "device.toml"))
    ap.add_argument("--rel", default=REL_DIR, help="directory of the signed packages <chip>.wlpkg")
    ap.add_argument("--backups", default=BACKUP_DIR)
    ap.add_argument("--yes", action="store_true", help="really write (to the board, the phone)")
    a = ap.parse_args(argv)
    cfg, board = load(a.config, a.board)
    cfg = dict(cfg, rel=a.rel, backups=a.backups)
    lab = Lab(board, cfg)
    steps = {"check": lab.check, "backup": lab.backup, "prepare": lambda: lab.prepare(a.yes), "push": lambda: lab.push(a.yes),
             "flash": lambda: lab.flash(a.yes), "verify": lab.verify, "join": lambda: lab.join(a.yes)}
    if a.step == "all":
        if not a.yes:
            sys.exit("`all` writes to the board: add --yes (try the steps one by one first)")
        for s in ("check", "backup", "prepare", "push", "flash", "verify", "join"):
            steps[s]()
    else:
        steps[a.step]()


if __name__ == "__main__":
    main()

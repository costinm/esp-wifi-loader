import hashlib
import os
import subprocess
import time
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
sys.path.insert(0, os.path.dirname(__file__))
import sign
import reset
import upload
import provision
import wl_format as wf
from ref_device import Device

IMAGE = bytes([0xE9, 0, 2, 0x10]) + os.urandom(10000)


class Tools(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.mkdtemp()
        cls.k1, cls.k2 = (os.path.join(cls.dir, n) for n in ("a.pem", "b.pem"))
        sign.keygen(cls.k1)
        sign.keygen(cls.k2)

    def pkg(self, key=None, version=5, image=IMAGE, cpu="esp32c6", target="main"):
        return sign.make_package(key or self.k1, cpu, version, "dl.example.org", image, target)

    def test_header_size_and_roundtrip(self):
        pkg = self.pkg()
        pub, sig, hdr, table, image = wf.split_package(pkg)
        self.assertEqual(len(pub), 65)
        self.assertEqual(len(hdr), 216)
        self.assertEqual(image, IMAGE)
        self.assertEqual(len(table), 8 * 3)
        self.assertEqual(wf.unpack_header(hdr)["fqdn"], "dl.example.org")

    def test_install_and_tofu(self):
        d = Device()
        self.assertTrue(d.upload(self.pkg()).startswith("OK"))
        self.assertEqual(d.main, IMAGE)
        self.assertEqual(d.upload(self.pkg(key=self.k2, version=6)), "ERR 8 key-mismatch")
        self.assertTrue(d.upload(self.pkg(version=6)).startswith("OK"))

    def test_trust_anchor_replaces_tofu(self):
        d = Device(anchor=sign.public_key(self.k1))
        self.assertEqual(d.upload(self.pkg(key=self.k2)), "ERR 8 key-mismatch")
        self.assertTrue(d.upload(self.pkg()).startswith("OK"))
        self.assertIsNone(d.pin)

    def test_trust_slot_in_the_stage2_image(self):
        pub = sign.public_key(self.k1)
        boot = os.urandom(19584)
        for offset in (0, 0x1000):
            img = wf.embed_trust_key(boot, offset, pub)
            self.assertEqual(len(img), wf.TRUST_OFFSET - offset + wf.TRUST_SIZE)
            self.assertEqual(img[:len(boot)], boot)
            self.assertEqual(wf.parse_trust_slot(img[wf.TRUST_OFFSET - offset:]), pub)
            self.assertLessEqual(offset + len(img), wf.STAGE2_REGION_END)
        self.assertIsNone(wf.parse_trust_slot(b"\xff" * 256))
        with self.assertRaises(ValueError):
            wf.embed_trust_key(b"\0" * 0x7000, 0, pub)

    def test_rollback_and_chip(self):
        d = Device()
        d.upload(self.pkg(version=5))
        self.assertEqual(d.upload(self.pkg(version=4)), "ERR 9 rollback")
        self.assertTrue(d.upload(self.pkg(version=5)).startswith("OK"))
        self.assertEqual(Device("esp32").upload(self.pkg()), "ERR 10 wrong-chip-or-target")

    def test_tamper(self):
        pkg = bytearray(self.pkg())
        d = Device()
        bad = bytearray(pkg); bad[129 + 20] ^= 1          # header byte
        self.assertEqual(d.upload(bytes(bad)), "ERR 7 bad-signature")
        bad = bytearray(pkg); bad[-1] ^= 1                # last image byte
        self.assertEqual(d.upload(bytes(bad)), "ERR 13 bad-block")
        bad = bytearray(pkg); bad[345] ^= 1               # block table
        self.assertEqual(d.upload(bytes(bad)), "ERR 12 bad-block-table")
        self.assertIsNone(d.main)

    def test_targets_by_mode(self):
        loader_dev, boot_dev = Device(), Device(mode="bootstrap")
        self.assertEqual(loader_dev.upload(self.pkg(target="loader")), "ERR 10 wrong-chip-or-target")
        self.assertEqual(boot_dev.upload(self.pkg(target="main")), "ERR 10 wrong-chip-or-target")
        self.assertTrue(boot_dev.upload(self.pkg(target="loader", version=3)).startswith("OK"))
        self.assertTrue(boot_dev.upload(self.pkg(target="stage2", version=1)).startswith("OK"))
        # versions are tracked per target
        self.assertEqual(boot_dev.upload(self.pkg(target="loader", version=2)), "ERR 9 rollback")
        self.assertTrue(boot_dev.upload(self.pkg(target="stage2", version=1)).startswith("OK"))
        big = IMAGE + bytes(0x7000)
        self.assertEqual(boot_dev.upload(self.pkg(target="stage2", image=big, version=2)), "ERR 3 bad-length")

    def test_zero_length_manifest_requests_the_loader(self):
        manifest = sign.make_package(self.k1, "esp32c6", 9, "", b"", "main")
        boot = Device(mode="bootstrap")
        self.assertEqual(boot.upload(manifest), "OK 0 request-loader")
        self.assertTrue(boot.loader_requested)
        # the loader has nothing to reboot to; a wrong key or an older version is refused
        self.assertEqual(Device().upload(manifest), "ERR 3 bad-length")
        boot.pin = sign.public_key(self.k1)
        self.assertEqual(boot.upload(sign.make_package(self.k2, "esp32c6", 9, "", b"", "main")), "ERR 8 key-mismatch")
        boot.minvers[1] = 10
        self.assertEqual(boot.upload(manifest), "ERR 9 rollback")

    def test_dry_run_changes_nothing(self):
        d = Device()
        self.assertTrue(d.upload(self.pkg(), dry=True).startswith("OK"))
        self.assertIsNone(d.pin)

    def test_openssl_verifies_signature(self):
        pkg = self.pkg()
        pub, sig, hdr, _, _ = wf.split_package(pkg)
        from ref_device import _verify
        self.assertTrue(_verify(pub, sig, hdr))
        self.assertFalse(_verify(pub, sig, hdr[:-1] + b"x"))

    def test_announce_parse(self):
        info = upload.parse_announce(
            "esp-wifi-loader v=1 state=blank version=0 key=none fqdn= chip=esp32c6 "
            "mac=aa:bb:cc:dd:ee:ff port=5227 role=ap loader=1 up=3")
        self.assertEqual(info["state"], "blank")
        self.assertEqual(info["role"], "ap")

    def test_transport_header(self):
        self.assertEqual(len(wf.transport_header()), 16)
        self.assertEqual(wf.transport_header(dry=True)[5], 2)
        self.assertEqual(wf.transport_header(reboot=True)[6], 255)


class FakeSerial:
    """Records the DTR/RTS line changes the way the chip sees them."""

    def __init__(self, log):
        self.log, self._dtr, self._rts = log, True, True      # an opened port has both lines asserted

    @property
    def dtr(self):
        return self._dtr

    @dtr.setter
    def dtr(self, v):
        self._dtr = v
        self.log.append(("dtr", v))

    @property
    def rts(self):
        return self._rts

    @rts.setter
    def rts(self, v):
        self._rts = v
        self.log.append(("rts", v))

    def reset_input_buffer(self):
        pass

    def read(self, n):
        time.sleep(0.001)
        return b""

    def close(self):
        pass


class Reset(unittest.TestCase):
    def port(self):
        log, sleeps = [], []
        return reset.Port("x", serial_factory=lambda p: FakeSerial(log), sleep=sleeps.append).open(), log, sleeps

    def test_one_reset_is_dtr_low_while_rts_is_high(self):
        p, log, _ = self.port()
        p.reset()
        # rts is already high, dtr goes low (the chip resets), then rts is released
        self.assertEqual(log, [("rts", True), ("dtr", False), ("rts", False)])

    def test_the_loader_sequence_is_five_resets_with_a_gap(self):
        p, log, sleeps = self.port()
        p.reset_times(reset.QUICK_RESETS, 0.7)
        self.assertEqual(sum(1 for l in log if l == ("dtr", False)), 5)
        self.assertEqual(sleeps.count(0.1), 5)                 # the reset pulse; the gaps are spent reading the console
        self.assertLess(4 * 0.7 + 5 * 0.1, 5.0)                # the whole sequence fits Stage2's five second window

    def test_to_loader_stops_at_the_banner(self):
        log, resets = [], []

        class Boots(FakeSerial):
            def read(self, n):
                # the third reset is the one Stage2 answers with the loader
                return b"W (200) wl: loader v1 main=valid" if sum(1 for l in log if l == ("dtr", False)) >= 3 else b"main"

        p = reset.Port("x", serial_factory=lambda path: Boots(log), sleep=lambda s: None).open()
        out = p.to_loader(0.02)
        self.assertIn("loader v1", out)
        self.assertEqual(sum(1 for l in log if l == ("dtr", False)), 3)     # no reset after the loader started

    def test_a_port_that_vanishes_is_reopened(self):
        log, opened = [], []

        class Flaky(FakeSerial):
            def __init__(self, n):
                super().__init__(log)
                self.n = n

            @property
            def rts(self):
                return self._rts

            @rts.setter
            def rts(self, v):
                if self.n == 0 and v is False:
                    raise OSError("device disconnected")        # USB re-enumeration right after the reset
                self._rts = v
                log.append(("rts", v))

        def factory(path):
            opened.append(path)
            return Flaky(len(opened) - 1)

        p = reset.Port("x", serial_factory=factory, sleep=lambda s: None).open()
        p.reset_times(2, 0.0)
        self.assertGreaterEqual(len(opened), 2)


class Provision(unittest.TestCase):
    """provision.py: what it reads from a board, and the rules that keep it from writing to the wrong one."""

    @staticmethod
    def sector(seq, records):
        sec = bytearray(b"\xff" * 4096)
        sec[0:16] = b"WLS1" + seq.to_bytes(4, "little") + (0xA5A5A5A5).to_bytes(4, "little") + b"\xff" * 4
        off = 16
        for typ, data in records:
            crc = provision._crc16(provision._crc16(0xFFFF, bytes([typ, len(data)])), data)
            sec[off:off + 4] = (typ | len(data) << 8 | crc << 16).to_bytes(4, "little")
            sec[off + 4:off + 4 + len(data)] = data
            off += 4 + ((len(data) + 3) & ~3)
        return bytes(sec)

    def test_state_log_decoding(self):
        pub = b"\x04" + bytes(range(64))
        raw = self.sector(1, [(1, pub), (2, (7).to_bytes(4, "little")), (4, b"\x01"), (2, (9).to_bytes(4, "little"))]) + b"\xff" * 4096
        view = provision.state_log(raw)
        self.assertEqual(view["pin"], pub)
        self.assertEqual(provision.describe_state(view),
                         "key pinned: %s, main version >= 9, qio ok" % hashlib.sha256(pub).hexdigest()[:8])

    def test_newer_sector_wins_and_blank_is_blank(self):
        a = self.sector(5, [(2, (1).to_bytes(4, "little"))])
        b = self.sector(6, [(2, (2).to_bytes(4, "little"))])
        self.assertEqual(provision.state_log(a + b)["main_min_version"], (2).to_bytes(4, "little"))
        self.assertEqual(provision.state_log(b + a)["main_min_version"], (2).to_bytes(4, "little"))
        self.assertIsNone(provision.state_log(b"\xff" * 8192))
        self.assertEqual(provision.describe_state(None), "blank (no key pinned)")

    def test_corrupt_record_ends_the_log(self):
        raw = bytearray(self.sector(1, [(1, b"\x04" + bytes(64)), (2, (3).to_bytes(4, "little"))]))
        raw[16 + 4 + 68 + 4 + 1] ^= 1          # damage the second record's payload
        view = provision.state_log(bytes(raw) + b"\xff" * 4096)
        self.assertIn("pin", view)
        self.assertNotIn("main_min_version", view)

    def test_partition_table(self):
        import struct
        entry = lambda name, t, st, off, size: b"\xaa\x50" + struct.pack("<BBII", t, st, off, size) + name.ljust(16, b"\0") + bytes(4)
        raw = entry(b"loader", 0, 0, 0x10000, 0xE0000) + entry(b"main", 0, 16, 0xF0000, 0x2D0000) + b"\xff" * 64
        self.assertEqual([(n, o, z) for n, _, _, o, z in provision.partition_table(raw)],
                         [("loader", 0x10000, 0xE0000), ("main", 0xF0000, 0x2D0000)])
        self.assertTrue(provision.image_head(b"\xe9\x05" + bytes(10) + (13).to_bytes(2, "little") + bytes(2)).startswith("valid image (chip id 13"))
        self.assertTrue(provision.image_head(b"\xff" * 16).startswith("no valid image"))

    def lab(self, mac_reported):
        calls = []

        def run(cmd, **kw):
            calls.append(cmd)
            text = "Chip type: ESP32-C6\nMAC: " + mac_reported + "\n" if "chip-id" in cmd else ""
            return type("R", (), {"returncode": 0, "stdout": text})()
        board = dict(name="t", chip="esp32c6", mac="10:bd:a3:ac:5a:20", port="/dev/null", phone="PH")
        return provision.Lab(board, {}, run=run), calls

    def test_the_mac_is_checked_and_usb_jtag_eui64_is_understood(self):
        lab, _ = self.lab("10:bd:a3:ff:fe:ac:5a:20")
        self.assertEqual(lab.identity(), "ESP32-C6")
        lab, _ = self.lab("10:bd:a3:ac:5a:21")
        with self.assertRaises(SystemExit) as cm:
            lab.identity()
        self.assertIn("refusing to touch", str(cm.exception))

    def test_nothing_is_written_without_yes(self):
        lab, calls = self.lab("10:bd:a3:ac:5a:20")
        lab.prepare(False)
        lab.flash(False)
        written = [c for c in calls if any(w in c for w in ("erase-region", "write-flash", "push", "install"))]
        self.assertEqual(written, [])
        self.assertEqual(calls, [])          # a dry run does not even talk to the board

    def test_prepare_wants_a_backup_first(self):
        lab, calls = self.lab("10:bd:a3:ac:5a:20")
        lab.name = "no-such-board-has-a-backup"
        with self.assertRaises(SystemExit) as cm:
            lab.prepare(True)
        self.assertIn("backup", str(cm.exception))
        self.assertFalse([c for c in calls if "erase-region" in c])

    def test_a_stale_boot_target_is_found(self):
        nvs = bytearray(b"\xff" * 0x6000)
        nvs[0xda8:0xdad] = b"stg2\0"
        nvs[0xdc8:0xdd4] = b"boot_target\0"
        nvs[0xdd8:0xddc] = (1).to_bytes(4, "little")
        self.assertEqual(provision.boot_target(bytes(nvs)), 1)
        self.assertEqual(provision.boot_target(b"\xff" * 0x6000), 0)

    def test_the_access_point_name_follows_the_mac(self):
        lab, _ = self.lab("10:bd:a3:ac:5a:20")
        self.assertEqual(lab.ap_ssid(), "espmod-5a21")           # SoftAP MAC = base MAC + 1, last two bytes
        lab.b["mac"] = "10:bd:a3:ac:5a:ff"
        self.assertEqual(lab.ap_ssid(), "espmod-5a00")      # ESP-IDF adds 1 to the last byte only, no carry

    def test_join_and_flash_are_dry_runs_without_yes(self):
        lab, calls = self.lab("10:bd:a3:ac:5a:20")
        lab.join(False)
        lab.flash(False)
        self.assertEqual(calls, [])


if __name__ == "__main__":
    unittest.main()

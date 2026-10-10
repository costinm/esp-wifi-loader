import os
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
sys.path.insert(0, os.path.dirname(__file__))
import sign
import upload
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


if __name__ == "__main__":
    unittest.main()

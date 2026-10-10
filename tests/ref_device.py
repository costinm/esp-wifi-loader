"""Python reference of the device-side checks in upload.c, for protocol tests."""
import hashlib
import os
import subprocess
import tempfile

import wl_format as wf


def _verify(pub, sig, header):
    r, s = int.from_bytes(sig[:32], "big"), int.from_bytes(sig[32:], "big")
    def der_int(v):
        b = v.to_bytes(33, "big").lstrip(b"\0") or b"\0"
        if b[0] & 0x80:
            b = b"\0" + b
        return bytes([2, len(b)]) + b
    body = der_int(r) + der_int(s)
    der = bytes([0x30, len(body)]) + body
    spki_prefix = bytes.fromhex("3059301306072a8648ce3d020106082a8648ce3d030107034200")
    with tempfile.TemporaryDirectory() as d:
        for name, data in (("pub.der", spki_prefix + pub), ("sig", der), ("hdr", header)):
            open(os.path.join(d, name), "wb").write(data)
        p = subprocess.run(["openssl", "dgst", "-sha256", "-verify", os.path.join(d, "pub.der"),
                            "-keyform", "DER", "-signature", os.path.join(d, "sig"),
                            os.path.join(d, "hdr")], capture_output=True)
    return p.returncode == 0


class Device:
    def __init__(self, cpu="esp32c6", part_size=0x2d0000, mode="loader", anchor=None):
        self.mode = mode
        self.anchor = anchor  # public key from the Stage2 trust slot, or None (trust on first use)
        self.minvers = {}
        self.cpu = wf.CHIPS[cpu]
        self.size = part_size
        self.pin = None
        self.minver = None
        self.main = None

    def upload(self, pkg, dry=False):
        pub, sig, hdr, table, image = wf.split_package(pkg)
        h = wf.unpack_header(hdr)
        if h["magic"] != wf.MAGIC or not _verify(pub, sig, hdr):
            return "ERR 7 bad-signature"
        if (self.anchor or self.pin) and (self.anchor or self.pin) != pub:
            return "ERR 8 key-mismatch"
        allowed = (1,) if self.mode == "loader" else (2, 3)
        manifest = self.mode != "loader" and h["target"] == 1 and h["image_size"] == 0
        if self.minvers.get(h["target"]) is not None and h["version"] < self.minvers[h["target"]]:
            return "ERR 9 rollback"
        if h["format"] != 1 or h["cpu"] != self.cpu or (h["target"] not in allowed and not manifest):
            return "ERR 10 wrong-chip-or-target"
        if manifest:
            if h["block_count"] != 0:
                return "ERR 3 bad-length"
            if not dry:
                self.loader_requested = True
            return "OK 0 request-loader" + (" dry" if dry else "")
        limit = {1: self.size, 2: 0xe0000 - 8192, 3: 0x7000}[h["target"]]
        if not (4096 <= h["image_size"] <= limit) or h["block_size"] != 4096 or \
                h["block_count"] != -(-h["image_size"] // 4096):
            return "ERR 3 bad-length"
        if hashlib.sha256(table).digest() != h["table_sha256"]:
            return "ERR 12 bad-block-table"
        for i in range(h["block_count"]):
            blk = image[i * 4096:(i + 1) * 4096]
            if hashlib.sha256(blk).digest()[:8] != table[i * 8:i * 8 + 8]:
                return "ERR 13 bad-block"
        if hashlib.sha256(image).digest() != h["image_sha256"]:
            return "ERR 14 bad-image-hash"
        if not dry:
            self.pin = self.pin or (None if self.anchor else pub)
            self.minvers[h["target"]] = h["version"]
            self.minver = h["version"]
            self.written = getattr(self, "written", {})
            self.written[h["target"]] = image
            if h["target"] == 1:
                self.main = image
        return f"OK {len(image)}"

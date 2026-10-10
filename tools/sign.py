#!/usr/bin/env python3
"""Make keys and signed Main packages for esp-wifi-loader.

  sign.py keygen KEY.pem
  sign.py sign --key KEY.pem --cpu esp32c6 --version 12 --fqdn host.example \\
               --image main.bin --out main.wlpkg
  sign.py sign ... --target loader --image loader.bin   # for the bootstrap build
  sign.py sign ... --target stage2 --image stage2-4mb.bin
  sign.py sign --key KEY.pem --cpu esp32c6 --version 12 --request-loader --out upgrade.wlpkg
        # empty manifest: tells a running Main to reboot into the loader
  sign.py info main.wlpkg

Uses the `cryptography` package; falls back to the `openssl` command line
when it is not installed.
"""
import argparse
import subprocess
import sys

import wl_format as wf

try:
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.hazmat.primitives.asymmetric.utils import (
        decode_dss_signature, encode_dss_signature)
    HAVE_CRYPTO = True
except ImportError:
    HAVE_CRYPTO = False


def _openssl(*args, stdin=None):
    return subprocess.run(["openssl", *args], input=stdin, check=True,
                          stdout=subprocess.PIPE).stdout


def keygen(path):
    if HAVE_CRYPTO:
        key = ec.generate_private_key(ec.SECP256R1())
        pem = key.private_bytes(serialization.Encoding.PEM,
                                serialization.PrivateFormat.PKCS8,
                                serialization.NoEncryption())
        with open(path, "wb") as f:
            f.write(pem)
    else:
        _openssl("ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", path)


def public_key(path) -> bytes:
    """65-byte uncompressed P-256 point."""
    if HAVE_CRYPTO:
        with open(path, "rb") as f:
            key = serialization.load_pem_private_key(f.read(), None)
        return key.public_key().public_bytes(serialization.Encoding.X962,
                                             serialization.PublicFormat.UncompressedPoint)
    der = _openssl("ec", "-in", path, "-pubout", "-outform", "DER")
    return der[-65:]


def sign_raw(path, data: bytes) -> bytes:
    """ECDSA P-256 over SHA-256(data), 64 bytes r||s."""
    if HAVE_CRYPTO:
        with open(path, "rb") as f:
            key = serialization.load_pem_private_key(f.read(), None)
        r, s = decode_dss_signature(key.sign(data, ec.ECDSA(hashes.SHA256())))
    else:
        der = _openssl("dgst", "-sha256", "-sign", path, stdin=data)
        # SEQUENCE { INTEGER r, INTEGER s }
        i = 2 if der[1] < 0x80 else 3
        def take(i):
            n = der[i + 1]
            return int.from_bytes(der[i + 2:i + 2 + n], "big"), i + 2 + n
        r, i = take(i)
        s, _ = take(i)
    return r.to_bytes(32, "big") + s.to_bytes(32, "big")


def make_package(key_path, cpu, version, fqdn, image: bytes, target="main") -> bytes:
    table = wf.block_table(image)
    header = wf.pack_header(cpu, version, image, table, fqdn, wf.TARGETS[target])
    return public_key(key_path) + sign_raw(key_path, header) + header + table + image


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    k = sub.add_parser("keygen"); k.add_argument("key")
    s = sub.add_parser("sign")
    s.add_argument("--key", required=True)
    s.add_argument("--cpu", required=True, choices=sorted(wf.CHIPS))
    s.add_argument("--version", required=True, type=int)
    s.add_argument("--target", choices=sorted(wf.TARGETS), default="main",
                   help="main (loader mode), loader or stage2 (bootstrap mode)")
    s.add_argument("--fqdn", default="", help="download host announced for Main updates")
    s.add_argument("--image", help="image to sign (not with --request-loader)")
    s.add_argument("--request-loader", action="store_true",
                   help="zero-length Main manifest: a Main running the component reboots into the loader")
    s.add_argument("--out", required=True)
    i = sub.add_parser("info"); i.add_argument("package")
    a = ap.parse_args()
    if a.cmd == "keygen":
        keygen(a.key)
    elif a.cmd == "sign":
        if a.request_loader:
            image, a.target = b"", "main"
        elif a.image:
            with open(a.image, "rb") as f:
                image = f.read()
        else:
            ap.error("--image or --request-loader is required")
        pkg = make_package(a.key, a.cpu, a.version, a.fqdn, image, a.target)
        with open(a.out, "wb") as f:
            f.write(pkg)
        print(f"wrote {a.out} (target {a.target}): {len(pkg)} bytes, key={wf.key_fingerprint(pkg[:65])}")
    else:
        with open(a.package, "rb") as f:
            pub, _, hdr, table, image = wf.split_package(f.read())
        print(f"key={wf.key_fingerprint(pub)}", wf.unpack_header(hdr), f"image={len(image)}")


if __name__ == "__main__":
    sys.exit(main())

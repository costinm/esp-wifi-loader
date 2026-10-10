"""Shared wire-format helpers for esp-wifi-loader (docs/protocol.md)."""
import hashlib
import struct

MAGIC = 0x31484C57  # "WLH1"
FORMAT = 1
BLOCK = 4096
HEADER = struct.Struct("<IBBBBIIHH32s32sB3x128s")
assert HEADER.size == 216
TRANSPORT_MAGIC = b"WLU1"
CHIPS = {"esp32": 0, "esp32s3": 9, "esp32c6": 13}  # esp_chip_id_t
TARGETS = {"main": 1, "loader": 2, "stage2": 3, "module": 4}
TARGET_MAIN = 1

# Trust slot: the public key that signs loader, Stage2, Main and module packages, written into the Stage2 image
# at a fixed flash offset (the same on every chip). A board whose slot is erased (0xFF) has no anchor and
# falls back to pinning the first key it sees. Layout: "WLK1" | len u8 (65) | pad 3 | key[65] | 0xFF fill.
TRUST_OFFSET = 0x6F00
TRUST_SIZE = 256
TRUST_MAGIC = b"WLK1"
STAGE2_REGION_END = 0x7000


def trust_slot(pub: bytes) -> bytes:
    if len(pub) != 65 or pub[0] != 4:
        raise ValueError("trust key must be an uncompressed P-256 point (65 bytes)")
    return (TRUST_MAGIC + bytes([65, 0, 0, 0]) + pub).ljust(TRUST_SIZE, b"\xff")


def parse_trust_slot(slot: bytes):
    """The 65-byte key in a trust slot, or None when the slot is erased or malformed."""
    if len(slot) < 72 or slot[:4] != TRUST_MAGIC or slot[4] != 65 or slot[8] != 4:
        return None
    return bytes(slot[8:73])


def embed_trust_key(bootloader: bytes, boot_offset: int, pub: bytes) -> bytes:
    """Stage2 image with the trust slot at its fixed flash offset (padding in between is 0xFF)."""
    end = TRUST_OFFSET - boot_offset
    if len(bootloader) > end:
        raise ValueError(f"bootloader ({len(bootloader)} bytes) reaches the trust slot at 0x{TRUST_OFFSET:x}")
    return bootloader.ljust(end, b"\xff") + trust_slot(pub)


def block_table(image: bytes) -> bytes:
    """Truncated (8 byte) SHA-256 of every 4 KiB block; the last may be short."""
    return b"".join(hashlib.sha256(image[i:i + BLOCK]).digest()[:8]
                    for i in range(0, len(image), BLOCK))


def pack_header(cpu, version, image, table, fqdn, target=TARGET_MAIN):
    # fqdn: the download host of a Main package, or the module name for a module package
    name = fqdn.encode()
    if len(name) > 128:
        raise ValueError("fqdn longer than 128 bytes")
    return HEADER.pack(MAGIC, FORMAT, CHIPS[cpu], target, 0, version, len(image),
                       BLOCK, len(table) // 8, hashlib.sha256(image).digest(),
                       hashlib.sha256(table).digest(), len(name), name.ljust(128, b"\0"))


def unpack_header(raw: bytes) -> dict:
    (magic, fmt, cpu, target, flags, version, size, bsize, bcount, isha, tsha,
     flen, fqdn) = HEADER.unpack(raw)
    return dict(magic=magic, format=fmt, cpu=cpu, target=target, flags=flags,
                version=version, image_size=size, block_size=bsize, block_count=bcount,
                image_sha256=isha, table_sha256=tsha, fqdn=fqdn[:flen].decode())


def transport_header(dry=False, reboot=False) -> bytes:
    return TRANSPORT_MAGIC + bytes([FORMAT, 2 if dry else 0, 255 if reboot else 0, 0]) + bytes(8)


def key_fingerprint(pub: bytes) -> str:
    """The `key=` announce field: first 4 bytes of SHA-256 of the public key."""
    return hashlib.sha256(pub).digest()[:4].hex()


# A signed package is the upload body that follows the transport header:
#   public key (65) | signature (64) | header (216) | block table | image
def split_package(pkg: bytes):
    pub, sig, hdr = pkg[:65], pkg[65:129], pkg[129:129 + 216]
    h = unpack_header(hdr)
    tlen = h["block_count"] * 8
    return pub, sig, hdr, pkg[345:345 + tlen], pkg[345 + tlen:]

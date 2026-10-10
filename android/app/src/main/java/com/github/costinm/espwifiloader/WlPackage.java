package com.github.costinm.espwifiloader;

import java.util.Arrays;

/** A signed package: public key | signature | header | block table | image. */
public final class WlPackage {
    public final byte[] bytes;
    public final WlHeader header;

    private WlPackage(byte[] bytes, WlHeader header) {
        this.bytes = bytes;
        this.header = header;
    }

    public static WlPackage parse(byte[] bytes) {
        if (bytes.length < Wl.PACKAGE_FIXED) throw new IllegalArgumentException("package too short");
        return new WlPackage(bytes, new WlHeader(Arrays.copyOfRange(bytes,
                Wl.PUBKEY_SIZE + Wl.SIG_SIZE, Wl.PACKAGE_FIXED)));
    }

    public byte[] pub() { return Arrays.copyOfRange(bytes, 0, Wl.PUBKEY_SIZE); }
    public byte[] sig() { return Arrays.copyOfRange(bytes, Wl.PUBKEY_SIZE, Wl.PUBKEY_SIZE + Wl.SIG_SIZE); }
    public byte[] table() {
        return Arrays.copyOfRange(bytes, Wl.PACKAGE_FIXED, Wl.PACKAGE_FIXED + header.blockCount * 8);
    }
    public byte[] image() { return Arrays.copyOfRange(bytes, Wl.PACKAGE_FIXED + header.blockCount * 8, bytes.length); }
    public String keyFingerprint() { return Wl.keyFingerprint(pub()); }

    /** Same checks the device makes before it writes; null when valid. */
    public String verify() {
        WlHeader h = header;
        if (h.magic != Wl.MAGIC || h.format != Wl.FORMAT) return "bad-header";
        if (!Wl.verifySignature(pub(), sig(), h.raw)) return "bad-signature";
        if (h.blockSize != Wl.BLOCK || h.blockCount != (h.imageSize + Wl.BLOCK - 1) / Wl.BLOCK) return "bad-length";
        if (bytes.length != Wl.PACKAGE_FIXED + h.blockCount * 8L + h.imageSize) return "bad-length";
        byte[] table = table();
        if (!Arrays.equals(Wl.sha256(table), h.tableSha)) return "bad-block-table";
        byte[] img = image();
        for (int i = 0; i < h.blockCount; i++) {
            byte[] blk = Arrays.copyOfRange(img, i * Wl.BLOCK, Math.min((i + 1) * Wl.BLOCK, img.length));
            if (!Arrays.equals(Arrays.copyOf(Wl.sha256(blk), 8), Arrays.copyOfRange(table, i * 8, i * 8 + 8))) {
                return "bad-block";
            }
        }
        if (!Arrays.equals(Wl.sha256(img), h.imageSha)) return "bad-image-hash";
        return null;
    }

    /** Whether a package may be sent to the announcing device; null when it may. */
    public String refusal(Announce a) {
        WlHeader h = header;
        Integer chip = Wl.CHIPS.get(a.chip());
        if (chip == null || chip != h.cpu) return "package is for another chip";
        // Where Main runs (the bootstrap build, or an application with the component linked in):
        // loader and Stage2 images, plus the zero-length Main manifest that reboots into the loader.
        boolean mainRuns = a.mode().equals("bootstrap") || a.mode().equals("main");
        boolean manifest = h.target == Wl.TARGET_MAIN && h.imageSize == 0;
        boolean ok = mainRuns ? (h.target == Wl.TARGET_LOADER || h.target == Wl.TARGET_STAGE2 || manifest)
                              : h.target == Wl.TARGET_MAIN && !manifest;
        if (!ok) return "device in " + a.mode() + " mode does not take target " + h.target;
        // The loader key signs the loader and Stage2; Main and modules use the pinned key (the loader key is
        // accepted for them as well: the recovery image).
        String fp = keyFingerprint();
        boolean boot = h.target == Wl.TARGET_LOADER || h.target == Wl.TARGET_STAGE2;
        boolean pinned = a.key().equals("none") || a.key().equals(fp);
        boolean anchored = a.anchor().equals(fp);
        if (boot && !a.anchor().equals("none")) {
            if (!anchored) return "not signed by the device's loader key";
        } else if (!pinned && !anchored) {
            return "signed by a different key than the device pins";
        }
        if (h.target == Wl.TARGET_MAIN && !manifest && h.version < a.version()) {
            return "older than the installed version " + a.version();
        }
        return null;
    }
}

package com.github.costinm.espwifiloader;

import java.math.BigInteger;
import java.security.AlgorithmParameters;
import java.security.KeyFactory;
import java.security.MessageDigest;
import java.security.Signature;
import java.security.spec.ECGenParameterSpec;
import java.security.spec.ECParameterSpec;
import java.security.spec.ECPoint;
import java.security.spec.ECPublicKeySpec;
import java.util.Map;

/** Wire format of docs/protocol.md. Plain Java: no Android classes. */
public final class Wl {
    private Wl() {}

    public static final int PORT = 5227;
    public static final String GROUP = "ff02::5227";
    public static final int FORMAT = 1;
    public static final long MAGIC = 0x31484C57L; // "WLH1"
    public static final int PUBKEY_SIZE = 65;
    public static final int SIG_SIZE = 64;
    public static final int HEADER_SIZE = 216;
    public static final int BLOCK = 4096;
    public static final int PACKAGE_FIXED = PUBKEY_SIZE + SIG_SIZE + HEADER_SIZE; // 345

    /** esp_chip_id_t */
    public static final Map<String, Integer> CHIPS = Map.of("esp32", 0, "esp32s3", 9, "esp32c6", 13);
    public static final int TARGET_MAIN = 1;
    public static final int TARGET_LOADER = 2;
    public static final int TARGET_STAGE2 = 3;

    public static byte[] sha256(byte[]... parts) {
        try {
            MessageDigest d = MessageDigest.getInstance("SHA-256");
            for (byte[] p : parts) d.update(p);
            return d.digest();
        } catch (java.security.NoSuchAlgorithmException e) {
            throw new IllegalStateException(e);
        }
    }

    public static String hex(byte[] b, int n) {
        StringBuilder s = new StringBuilder();
        for (int i = 0; i < n; i++) s.append(String.format("%02x", b[i]));
        return s.toString();
    }

    /** `key=` in the announce: first 4 bytes of SHA-256 of the public key. */
    public static String keyFingerprint(byte[] pub) {
        return hex(sha256(pub), 4);
    }

    public static byte[] transportHeader(boolean dry, boolean reboot) {
        byte[] h = new byte[16];
        byte[] magic = "WLU1".getBytes(java.nio.charset.StandardCharsets.US_ASCII);
        System.arraycopy(magic, 0, h, 0, 4);
        h[4] = FORMAT;
        h[5] = (byte) (dry ? 2 : 0);
        h[6] = (byte) (reboot ? 255 : 0);
        return h;
    }

    private static ECParameterSpec curve;

    private static synchronized ECParameterSpec curve() throws java.security.GeneralSecurityException {
        if (curve == null) {
            AlgorithmParameters p = AlgorithmParameters.getInstance("EC");
            p.init(new ECGenParameterSpec("secp256r1"));
            curve = p.getParameterSpec(ECParameterSpec.class);
        }
        return curve;
    }

    private static byte[] der(byte[] raw, int off) {
        byte[] b = new BigInteger(1, java.util.Arrays.copyOfRange(raw, off, off + 32)).toByteArray();
        byte[] out = new byte[b.length + 2];
        out[0] = 2;
        out[1] = (byte) b.length;
        System.arraycopy(b, 0, out, 2, b.length);
        return out;
    }

    /** Raw r||s -> DER, as java.security expects. */
    private static byte[] derSignature(byte[] raw) {
        byte[] r = der(raw, 0), s = der(raw, 32);
        byte[] out = new byte[r.length + s.length + 2];
        out[0] = 0x30;
        out[1] = (byte) (r.length + s.length);
        System.arraycopy(r, 0, out, 2, r.length);
        System.arraycopy(s, 0, out, 2 + r.length, s.length);
        return out;
    }

    public static boolean verifySignature(byte[] pub, byte[] sig, byte[] data) {
        try {
            if (pub.length != PUBKEY_SIZE || pub[0] != 4 || sig.length != SIG_SIZE) return false;
            ECPoint point = new ECPoint(new BigInteger(1, java.util.Arrays.copyOfRange(pub, 1, 33)),
                    new BigInteger(1, java.util.Arrays.copyOfRange(pub, 33, 65)));
            java.security.PublicKey key = KeyFactory.getInstance("EC")
                    .generatePublic(new ECPublicKeySpec(point, curve()));
            Signature s = Signature.getInstance("SHA256withECDSA");
            s.initVerify(key);
            s.update(data);
            return s.verify(derSignature(sig));
        } catch (java.security.GeneralSecurityException | RuntimeException e) {
            return false;
        }
    }
}

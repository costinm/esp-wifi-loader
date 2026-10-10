package com.github.costinm.espwifiloader;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;

/** The 216-byte signed header. */
public final class WlHeader {
    public final byte[] raw;
    public final long magic;
    public final int format, cpu, target, blockSize, blockCount;
    public final long version, imageSize;
    public final byte[] imageSha, tableSha;
    public final String fqdn;

    public WlHeader(byte[] raw) {
        if (raw.length != Wl.HEADER_SIZE) throw new IllegalArgumentException("header size");
        this.raw = raw;
        ByteBuffer b = ByteBuffer.wrap(raw).order(ByteOrder.LITTLE_ENDIAN);
        magic = b.getInt(0) & 0xffffffffL;
        format = raw[4] & 0xff;
        cpu = raw[5] & 0xff;
        target = raw[6] & 0xff;
        version = b.getInt(8) & 0xffffffffL;
        imageSize = b.getInt(12) & 0xffffffffL;
        blockSize = b.getShort(16) & 0xffff;
        blockCount = b.getShort(18) & 0xffff;
        imageSha = Arrays.copyOfRange(raw, 20, 52);
        tableSha = Arrays.copyOfRange(raw, 52, 84);
        fqdn = new String(raw, 88, Math.min(raw[84] & 0xff, 128), StandardCharsets.US_ASCII);
    }
}

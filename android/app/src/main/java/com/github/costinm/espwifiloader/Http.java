package com.github.costinm.espwifiloader;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;

public final class Http {
    private Http() {}

    /** Downloads a package (size-capped). */
    public static byte[] fetch(String url) throws IOException {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        c.setConnectTimeout(10_000);
        c.setReadTimeout(30_000);
        try (InputStream in = c.getInputStream()) {
            return readCapped(in, 8 << 20);
        }
    }

    public static byte[] readCapped(InputStream in, int max) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buf = new byte[8192];
        int n;
        while ((n = in.read(buf)) >= 0) {
            out.write(buf, 0, n);
            if (out.size() > max) throw new IOException("package larger than " + max + " bytes");
        }
        return out.toByteArray();
    }
}

package com.github.costinm.espwifiloader;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.function.Consumer;

public final class Uploader {
    private Uploader() {}

    /**
     * Sends transport header + package and reads the device's text replies.
     * The client FIN after the last byte ends the upload (docs/protocol.md).
     */
    public static UploadResult upload(InetSocketAddress to, byte[] pkg, boolean dry, boolean reboot,
                                      int timeoutMs, Consumer<String> progress) throws IOException {
        try (Socket s = new Socket()) {
            s.setSoTimeout(timeoutMs);
            s.connect(to, 10_000);
            UploadResult[] result = new UploadResult[1];
            // Device progress lines arrive while we are still sending.
            Thread reader = new Thread(() -> {
                try {
                    BufferedReader r = new BufferedReader(
                            new InputStreamReader(s.getInputStream(), StandardCharsets.US_ASCII));
                    String line;
                    while ((line = r.readLine()) != null) {
                        progress.accept(line);
                        if (line.startsWith("OK")) { result[0] = new UploadResult(true, line); break; }
                        if (line.startsWith("ERR")) { result[0] = new UploadResult(false, line); break; }
                    }
                } catch (IOException ignored) {
                    // reported as ERR no-reply
                }
            });
            reader.setDaemon(true);
            reader.start();
            try {
                OutputStream out = s.getOutputStream();
                out.write(Wl.transportHeader(dry, reboot));
                if (pkg != null) out.write(pkg);
                out.flush();
                s.shutdownOutput();
            } catch (IOException e) {
                // The device may reject early and close; its ERR line explains why.
            }
            try {
                reader.join(timeoutMs);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
            return result[0] != null ? result[0] : new UploadResult(false, "ERR no-reply");
        }
    }
}

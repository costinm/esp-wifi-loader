package com.github.costinm.espwifiloader;

import static org.junit.Assert.*;

import org.junit.Test;

import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/** sample.wlpkg is made by tools/sign.py (see src/test/resources/README). */
public class WlTest {
    private WlPackage sample() throws Exception {
        try (java.io.InputStream in = getClass().getResourceAsStream("/sample.wlpkg")) {
            return WlPackage.parse(in.readAllBytes());
        }
    }

    private static final String LINE = "esp-wifi-loader v=1 mode=loader state=blank version=0 key=none fqdn= "
            + "chip=esp32c6 mac=aa:bb:cc:dd:ee:ff port=5227 role=ap loader=1 up=3";

    @Test public void parsesAndVerifiesSignedPackage() throws Exception {
        WlPackage p = sample();
        assertNull(p.verify());
        assertEquals((int) Wl.CHIPS.get("esp32c6"), p.header.cpu);
        assertEquals(Wl.TARGET_MAIN, p.header.target);
        assertEquals(7L, p.header.version);
        assertEquals("dl.example.org", p.header.fqdn);
        assertEquals(5000, p.image().length);
    }

    @Test public void tamperingIsRejected() throws Exception {
        byte[] bytes = sample().bytes;
        assertEquals("bad-signature", flip(bytes, Wl.PUBKEY_SIZE + Wl.SIG_SIZE + 12).verify()); // header
        assertEquals("bad-block-table", flip(bytes, Wl.PACKAGE_FIXED).verify());                 // table
        assertEquals("bad-block", flip(bytes, bytes.length - 1).verify());                       // image
    }

    private static WlPackage flip(byte[] bytes, int at) {
        byte[] c = bytes.clone();
        c[at] ^= 1;
        return WlPackage.parse(c);
    }

    @Test public void announceParseAndRefusal() throws Exception {
        WlPackage p = sample();
        Announce a = Announce.parse(LINE);
        assertEquals("blank", a.state());
        assertNull(p.refusal(a));
        assertNotNull(p.refusal(Announce.parse(LINE.replace("esp32c6", "esp32"))));
        assertNotNull(p.refusal(Announce.parse(LINE.replace("key=none", "key=deadbeef"))));
        assertNull(p.refusal(Announce.parse(LINE.replace("key=none", "key=" + p.keyFingerprint()))));
        assertNotNull(p.refusal(Announce.parse(LINE.replace("version=0", "version=9")))); // downgrade
        assertNotNull(p.refusal(Announce.parse(LINE.replace("mode=loader", "mode=bootstrap"))));
        assertNull(Announce.parse("something else"));
    }

    private static ServerSocket server() throws Exception {
        return new ServerSocket(0, 1, InetAddress.getLoopbackAddress());
    }

    @Test public void uploadSendsHeaderThenPackageThenFin() throws Exception {
        WlPackage p = sample();
        try (ServerSocket server = server()) {
            byte[][] got = new byte[1][];
            Thread dev = new Thread(() -> {
                try (java.net.Socket c = server.accept()) {
                    got[0] = c.getInputStream().readAllBytes();   // ends at the client FIN
                    c.getOutputStream().write(("P " + got[0].length + " 0\nOK 5000\n").getBytes());
                } catch (java.io.IOException e) { throw new RuntimeException(e); }
            });
            dev.start();
            List<String> seen = new ArrayList<>();
            UploadResult r = Uploader.upload(new InetSocketAddress(InetAddress.getLoopbackAddress(),
                    server.getLocalPort()), p.bytes, false, false, 30_000, seen::add);
            dev.join();
            assertTrue(r.line, r.ok);
            assertEquals(16 + p.bytes.length, got[0].length);
            assertEquals("WLU1", new String(got[0], 0, 4));
            assertArrayEquals(p.bytes, Arrays.copyOfRange(got[0], 16, got[0].length));
            assertEquals(2, seen.size());
        }
    }

    @Test public void deviceErrorIsReported() throws Exception {
        try (ServerSocket server = server()) {
            Thread dev = new Thread(() -> {
                try (java.net.Socket c = server.accept()) {
                    c.getInputStream().readAllBytes();
                    c.getOutputStream().write("ERR 8 key-mismatch\n".getBytes());
                } catch (java.io.IOException e) { throw new RuntimeException(e); }
            });
            dev.start();
            UploadResult r = Uploader.upload(new InetSocketAddress(InetAddress.getLoopbackAddress(),
                    server.getLocalPort()), sample().bytes, false, false, 30_000, s -> { });
            dev.join();
            assertFalse(r.ok);
            assertEquals("ERR 8 key-mismatch", r.line);
        }
    }

    @Test public void packageSourceFromAnnounce() throws Exception {
        Flasher f = new Flasher(s -> { });
        String[] url = new String[1];
        byte[] bytes = sample().bytes;
        f.packageFor(Announce.parse("esp-wifi-loader v=1 state=update chip=esp32c6 mac=m fqdn=dl.example.org"),
                u -> { url[0] = u; return bytes; });
        assertEquals("https://dl.example.org/esp32c6.wlpkg", url[0]);
        f.template = "https://x/{chip}.wlpkg";
        f.packageFor(Announce.parse("esp-wifi-loader chip=esp32 mac=m"), u -> { url[0] = u; return bytes; });
        assertEquals("https://x/esp32.wlpkg", url[0]);
    }

    @Test public void sourceTextBecomesTemplate() {
        assertEquals("", Flasher.normalizeTemplate("  "));
        assertEquals("https://example.org/{chip}.wlpkg", Flasher.normalizeTemplate("example.org"));
        assertEquals("https://example.org/{chip}.wlpkg", Flasher.normalizeTemplate(" example.org/ "));
        assertEquals("https://h/x/{chip}.bin", Flasher.normalizeTemplate("https://h/x/{chip}.bin"));
    }

    @Test public void bundledPackagesAreTheFallbackAndCanBeForced() throws Exception {
        Flasher f = new Flasher(s -> { });
        byte[] bytes = sample().bytes;
        java.util.List<String> asked = new ArrayList<>();
        f.assets = n -> { asked.add(n); return bytes; };
        Flasher.Fetcher never = u -> { throw new AssertionError("no download expected: " + u); };
        f.packageFor(Announce.parse("esp-wifi-loader mode=loader chip=esp32c6 mac=m"), never);
        f.packageFor(Announce.parse("esp-wifi-loader mode=bootstrap chip=esp32c6 mac=m"), never);
        f.bundledKind = "stage2-4mb";
        f.packageFor(Announce.parse("esp-wifi-loader mode=bootstrap chip=esp32c6 mac=m fqdn=x.org"), never);
        assertEquals(Arrays.asList("esp32c6-main.wlpkg", "esp32c6-loader.wlpkg", "esp32c6-stage2-4mb.wlpkg"), asked);
        f.assets = n -> null;
        try { f.packageFor(Announce.parse("esp-wifi-loader chip=esp32 mac=m"), never); fail(); }
        catch (IllegalStateException expected) { }
    }

    @Test public void manifestIsOnlyForDevicesRunningMain() throws Exception {
        byte[] m = java.nio.file.Files.readAllBytes(java.nio.file.Paths.get(
                System.getProperty("wl.manifest", "src/test/resources/manifest.wlpkg")));
        WlPackage p = WlPackage.parse(m);
        assertNull(p.verify());
        assertEquals(0L, p.header.imageSize);
        assertNull(p.refusal(Announce.parse("esp-wifi-loader mode=main chip=esp32c6 mac=m key=none version=3")));
        assertNull(p.refusal(Announce.parse("esp-wifi-loader mode=bootstrap chip=esp32c6 mac=m")));
        assertNotNull(p.refusal(Announce.parse("esp-wifi-loader mode=loader chip=esp32c6 mac=m")));
    }
}

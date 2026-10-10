package com.github.costinm.espwifiloader;

import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.util.Collection;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.function.Consumer;

/** Tracks announcing devices and sends them a verified package. No Android classes. */
public final class Flasher {
    /** Fetches a package by URL (replaceable in tests). */
    public interface Fetcher { byte[] fetch(String url) throws Exception; }

    public static final class DeviceInfo {
        public final Announce announce;
        public final InetAddress addr;
        public final long seen;
        public final String status;

        DeviceInfo(Announce announce, InetAddress addr, long seen, String status) {
            this.announce = announce;
            this.addr = addr;
            this.seen = seen;
            this.status = status;
        }
    }

    private final ConcurrentHashMap<String, DeviceInfo> devices = new ConcurrentHashMap<>();
    private final Set<String> busy = ConcurrentHashMap.newKeySet();
    private final Consumer<String> log;

    /** Package picked from storage; used for every device when set. */
    public volatile WlPackage localPackage;
    /** For devices that announce no fqdn (blank): {@code https://host/{chip}.wlpkg}. */
    public volatile String template = "";

    public void setTemplate(String input) { template = normalizeTemplate(input); }

    /**
     * Packages bundled in the app (assets/packages/&lt;chip&gt;-&lt;kind&gt;.wlpkg; null when absent):
     * main (the bootstrap build), loader, stage2-4mb, stage2-8mb.
     */
    public volatile java.util.function.Function<String, byte[]> assets = name -> null;
    /** Forces one bundled kind for every device; empty = decide per device. */
    public volatile String bundledKind = "";

    private WlPackage bundled(Announce a, String kind) {
        byte[] b = assets.apply(a.chip() + "-" + kind + ".wlpkg");
        return b == null ? null : WlPackage.parse(b);
    }

    public Flasher(Consumer<String> log) { this.log = log; }

    public Collection<DeviceInfo> devices() { return devices.values(); }

    public int busyCount() { return busy.size(); }
    public boolean isAnyBusy() { return !busy.isEmpty(); }
    public boolean isBusy(String mac) { return busy.contains(mac); }

    /** Directory under a bare host where a site publishes its packages ({@code https://host/fw/<chip>.wlpkg}). */
    static final String SITE_PATH = "fw";
    private static final java.util.regex.Pattern GITHUB =
            java.util.regex.Pattern.compile("^([A-Za-z0-9][A-Za-z0-9-]*)/([A-Za-z0-9._-]+)(?::([A-Za-z0-9._-]+))?$");

    /**
     * Where packages come from, as the user types it or a device announces it (its {@code fqdn}). Returns a URL
     * template ending in {@code {chip}.wlpkg}, or "" for empty input:
     * <ul>
     * <li>{@code owner/repo} or {@code owner/repo:tag}: assets of a GitHub release, the tag defaulting to
     *     {@code latest} (the rolling release the CI publishes);</li>
     * <li>a bare host ({@code example.com}): {@code https://example.com/fw/{chip}.wlpkg};</li>
     * <li>a host with a path, or a full URL: that path/URL, with {@code {chip}.wlpkg} appended unless the URL
     *     already contains {@code {chip}}.</li>
     * </ul>
     */
    public static String normalizeTemplate(String input) {
        String t = input == null ? "" : input.trim();
        if (t.isEmpty()) return "";
        if (t.contains("://")) {
            return t.contains("{chip}") ? t : t.replaceAll("/+$", "") + "/{chip}.wlpkg";
        }
        java.util.regex.Matcher m = GITHUB.matcher(t);
        // "example.com/dl" has a dot in its first part: a host with a path, not owner/repo.
        if (m.matches() && !m.group(1).contains(".")) {
            String tag = m.group(3) == null ? "latest" : m.group(3);
            return "https://github.com/" + m.group(1) + "/" + m.group(2) + "/releases/download/" + tag + "/{chip}.wlpkg";
        }
        if (t.contains("{chip}")) return "https://" + t;
        if (!t.contains("/")) return "https://" + t + "/" + SITE_PATH + "/{chip}.wlpkg";
        return "https://" + t.replaceAll("/+$", "") + "/{chip}.wlpkg";
    }

    /** The package URL for a chip and kind ("main" is {@code <chip>.wlpkg}, others {@code <chip>-<kind>.wlpkg}). */
    static String packageUrl(String template, String chip, String kind) {
        String file = kind.equals("main") ? chip + ".wlpkg" : chip + "-" + kind + ".wlpkg";
        return template.replace("{chip}.wlpkg", file).replace("{chip}", chip);
    }

    public void onAnnounce(Announce a, InetAddress addr) {
        DeviceInfo old = devices.get(a.mac());
        if (old == null) {
            log.accept("device " + a.mac() + " " + a.chip() + " mode=" + a.mode() + " state=" + a.state()
                    + " via " + a.role());
        }
        long now = System.currentTimeMillis();
        String status = old == null ? "" : old.status;
        if (status.startsWith(UPLOADED) && rebooted(old, a, now)) {
            // Fresh announce from the device after the upload: report what is running now.
            String target = status.substring(UPLOADED.length()).trim();
            boolean loaderAgain = a.mode().equals("loader") && target.startsWith("main");
            status = loaderAgain ? "uploaded, but back in the loader (state=" + a.state() + ")"
                    : "running after upload: mode=" + a.mode() + " state=" + a.state() + " v=" + a.version();
            log.accept(a.mac() + ": " + status);
        }
        devices.put(a.mac(), new DeviceInfo(a, addr, now, status));
    }

    /** Status while the device has been sent an image but has not yet been seen running it. */
    static final String UPLOADED = "uploaded";
    /** The announce period of the loader is 5 s; a silence longer than this means it restarted. */
    private static final long RESTART_GAP_MS = 7_500;

    private static boolean rebooted(DeviceInfo old, Announce a, long now) {
        if (now - old.seen > RESTART_GAP_MS) return true;
        Announce o = old.announce;
        return !o.mode().equals(a.mode()) || !o.state().equals(a.state()) || o.version() != a.version();
    }

    void setStatus(String mac, String status) {
        devices.computeIfPresent(mac, (k, d) -> new DeviceInfo(d.announce, d.addr, d.seen, status));
    }

    WlPackage packageFor(Announce a, Fetcher fetch) throws Exception {
        WlPackage local = localPackage;
        if (local != null) return local;
        String forced = bundledKind;
        if (!forced.isEmpty()) {
            WlPackage p = bundled(a, forced);
            if (p == null) throw new IllegalStateException("no bundled " + forced + " package for " + a.chip());
            return p;
        }
        String url;
        String kind = a.mode().equals("bootstrap") ? "loader" : "main";
        if (!a.fqdn().isEmpty()) url = packageUrl(normalizeTemplate(a.fqdn()), a.chip(), kind);
        else if (!template.isEmpty()) url = packageUrl(template, a.chip(), kind);
        else {
            // Nothing to download from: use the bundled image this device mode takes.
            WlPackage p = bundled(a, a.mode().equals("bootstrap") ? "loader" : "main");
            if (p != null) return p;
            throw new IllegalStateException("no package: pick a file, set a domain or bundle packages");
        }
        log.accept("download " + url);
        return WlPackage.parse(fetch.fetch(url));
    }

    /** Verify, then send. Runs on its own thread; one upload per device at a time. */
    public void flash(String mac, boolean dry) {
        DeviceInfo d = devices.get(mac);
        if (d == null || !busy.add(mac)) return;
        Thread t = new Thread(() -> {
            try {
                setStatus(mac, "preparing");
                WlPackage pkg = packageFor(d.announce, Http::fetch);
                String bad = pkg.verify();
                if (bad != null) throw new IllegalStateException("package invalid: " + bad);
                String refused = pkg.refusal(d.announce);
                if (refused != null) throw new IllegalStateException(refused);
                setStatus(mac, "uploading");
                UploadResult r = Uploader.upload(new InetSocketAddress(d.addr, d.announce.port()),
                        pkg.bytes, dry, false, 30_000, line -> {
                            if (line.startsWith("P ")) setStatus(mac, "uploading " + line.split(" ")[1] + " B");
                        });
                log.accept(mac + ": " + r.line);
                // The OK line only answers the upload. Whether the new image boots and stays up is known
                // when the device announces again after its restart (onAnnounce); a dry run changes nothing.
                setStatus(mac, !r.ok ? "failed: " + r.line : dry ? "dry run ok"
                        : UPLOADED + " " + (pkg.header.target == Wl.TARGET_MAIN ? "main" : "image")
                        + " - waiting for the restart");
            } catch (Exception e) {
                log.accept(mac + ": " + e.getMessage());
                setStatus(mac, "failed: " + e.getMessage());
            } finally {
                busy.remove(mac);
            }
        }, "wl-flash-" + mac);
        t.setDaemon(true);
        t.start();
    }
}

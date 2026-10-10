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

    /**
     * What the user typed for a brand-new device: a bare domain ("example.org"), or a
     * URL that may contain {chip}. Returns the template, or "" for empty input.
     */
    public static String normalizeTemplate(String input) {
        String t = input == null ? "" : input.trim();
        if (t.isEmpty()) return "";
        if (!t.contains("://")) t = "https://" + t.replaceAll("/+$", "") + "/{chip}.wlpkg";
        return t;
    }

    public void onAnnounce(Announce a, InetAddress addr) {
        DeviceInfo old = devices.get(a.mac());
        if (old == null) {
            log.accept("device " + a.mac() + " " + a.chip() + " mode=" + a.mode() + " state=" + a.state()
                    + " via " + a.role());
        }
        devices.put(a.mac(), new DeviceInfo(a, addr, System.currentTimeMillis(), old == null ? "" : old.status));
    }

    private void setStatus(String mac, String status) {
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
        if (!a.fqdn().isEmpty()) url = "https://" + a.fqdn() + "/" + a.chip() + ".wlpkg";
        else if (!template.isEmpty()) url = template.replace("{chip}", a.chip());
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
                setStatus(mac, r.ok ? "done" : "failed: " + r.line);
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

package com.github.costinm.espwifiloader;

import android.content.Context;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.net.wifi.SoftApConfiguration;
import android.net.wifi.WifiManager;
import android.net.wifi.WifiNetworkSpecifier;
import android.net.wifi.WifiSsid;
import android.net.wifi.p2p.WifiP2pConfig;
import android.net.wifi.p2p.WifiP2pManager;
import android.os.Handler;
import android.os.Looper;

import java.nio.charset.StandardCharsets;
import java.util.function.Consumer;

/**
 * The phone's side of the flash networks.
 *
 * <ul>
 * <li>{@link #startFlashNetwork}: host the flasher network, DIRECT-xy-flash (WPA2, public passphrase) as a local-only
 *   hotspot. Android only lets an app choose the SSID from API 36.1; the Android 17
 *   Pixels tested ignored or refused the request (debugging.md). Experiment.</li>
 * <li>{@link #joinRescue}: join a device's ESP-RESCUE (WPA2, same passphrase). Works on
 *   any Android 10+ phone; the system asks the user to confirm.</li>
 * </ul>
 */
public final class Hotspot {
    private final WifiManager wifi;
    private final ConnectivityManager cm;
    private final Consumer<String> log;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final WifiManager.MulticastLock lock;
    private WifiManager.LocalOnlyHotspotReservation reservation;
    private ConnectivityManager.NetworkCallback rescueCallback;
    private final Context appContext;
    private WifiP2pManager p2p;
    private WifiP2pManager.Channel p2pChannel;
    private volatile boolean p2pGroup;
    private volatile String state = "off";
    private volatile long hostFailedAt;

    public Hotspot(Context ctx, Consumer<String> log) {
        Context app = ctx.getApplicationContext();
        this.appContext = app;
        this.wifi = (WifiManager) app.getSystemService(Context.WIFI_SERVICE);
        this.cm = app.getSystemService(ConnectivityManager.class);
        this.log = log;
        this.lock = wifi.createMulticastLock("esp-wifi-flasher"); // the announce is multicast
        this.lock.setReferenceCounted(false);
    }

    public String state() { return state; }

    /** Public constant shared with the firmware (CONFIG_WL_PASSPHRASE) and DMesh. */
    public static final String PASSPHRASE = "untrusted-open-mode";
    private static final int JOIN_TIMEOUT_MS = 25_000;
    public static final String FLASH_NAME = "flash";
    public static final String RESCUE_SSID = "ESP-RESCUE";
    /**
     * Android requires a Wi-Fi Direct group name of the form DIRECT-xy-...; we use "ef". The loader
     * accepts any two characters, so another flasher may choose its own.
     */
    private final String flashSsid = "DIRECT-ef-" + FLASH_NAME;

    public String flashSsid() { return flashSsid; }

    public void startFlashNetwork() {
        startFlashNetwork(flashSsid, PASSPHRASE);
    }

    public void startFlashNetwork(String ssid, String passphrase) {
        if (reservation != null || p2pGroup) return;
        lock.acquire();
        WifiManager.LocalOnlyHotspotCallback cb = new WifiManager.LocalOnlyHotspotCallback() {
            @Override public void onStarted(WifiManager.LocalOnlyHotspotReservation r) {
                reservation = r;
                SoftApConfiguration c = r.getSoftApConfiguration();
                state = "hosting " + c.getWifiSsid() + " security=" + c.getSecurityType();
                log.accept("hotspot started: " + state);
            }
            @Override public void onStopped() { reservation = null; state = "off"; log.accept("hotspot stopped"); }
            @Override public void onFailed(int reason) {
                hostFailedAt = System.currentTimeMillis();
                log.accept("local-only hotspot failed reason=" + reason + "; trying a Wi-Fi P2P group");
                startP2pGroup();
            }
        };
        try {
            SoftApConfiguration.Builder b = new SoftApConfiguration.Builder()
                    .setWifiSsid(WifiSsid.fromBytes(ssid.getBytes(StandardCharsets.UTF_8)))
                    // ESP32 radios are 2.4 GHz only; the phone would otherwise often pick 5 GHz.
                    .setChannels(channels24());
            if (passphrase == null) b.setPassphrase(null, SoftApConfiguration.SECURITY_TYPE_OPEN);
            else b.setPassphrase(passphrase, SoftApConfiguration.SECURITY_TYPE_WPA2_PSK);
            wifi.startLocalOnlyHotspotWithConfiguration(b.build(), handler::post, cb);
            state = "starting";
        } catch (Throwable t) { // NoSuchMethodError before 36.1, SecurityException without permission
            hostFailedAt = System.currentTimeMillis();
            log.accept("local-only hotspot unavailable (" + t.getClass().getSimpleName()
                    + "); trying a Wi-Fi P2P group");
            startP2pGroup();
        }
    }

    /**
     * Host the flash network as a Wi-Fi P2P group owner: an ordinary WPA2 access point to the ESP.
     * Works from Android 10 (API 29), where a freely named hotspot is not available to apps.
     * The name has to be DIRECT-xy-..., and the group is asked to use 2.4 GHz.
     */
    public void startP2pGroup() {
        startP2pGroup(flashSsid);
    }

    /** A group with another name, e.g. DIRECT-ef-dmesh, the one DMesh's Main joins by itself. */
    public void startP2pGroup(String name) {
        hostFailedAt = 0;
        try {
            if (p2p == null) {
                p2p = appContext.getSystemService(WifiP2pManager.class);
                p2pChannel = p2p == null ? null : p2p.initialize(appContext, Looper.getMainLooper(), null);
            }
            if (p2p == null || p2pChannel == null) throw new IllegalStateException("no Wi-Fi P2P");
            state = "starting";
            WifiP2pConfig cfg = new WifiP2pConfig.Builder()
                    .setNetworkName(name)
                    .setPassphrase(PASSPHRASE)
                    .setGroupOperatingBand(WifiP2pConfig.GROUP_OWNER_BAND_2GHZ)
                    .enablePersistentMode(false)
                    .build();
            // A group left from an earlier run would make createGroup fail with BUSY.
            p2p.removeGroup(p2pChannel, new WifiP2pManager.ActionListener() {
                @Override public void onSuccess() { createGroup(cfg, name); }
                @Override public void onFailure(int reason) { createGroup(cfg, name); }
            });
        } catch (RuntimeException e) {
            hostFailedAt = System.currentTimeMillis();
            state = "failed (P2P " + e.getClass().getSimpleName() + ": " + e.getMessage() + ")";
            log.accept("Wi-Fi P2P group: " + state);
        }
    }

    private void createGroup(WifiP2pConfig cfg, String name) {
        try {
            p2p.createGroup(p2pChannel, cfg, new WifiP2pManager.ActionListener() {
                @Override public void onSuccess() {
                    p2pGroup = true;
                    lock.acquire();
                    state = "hosting " + name + " (P2P group)";
                    log.accept("P2P group started: " + name);
                    p2p.requestGroupInfo(p2pChannel, g -> {
                        if (g != null) log.accept("P2P group " + g.getNetworkName() + " owner=" + g.isGroupOwner()
                                + " iface=" + g.getInterface());
                    });
                }
                @Override public void onFailure(int reason) {
                    hostFailedAt = System.currentTimeMillis();
                    state = "failed (P2P " + reason + ")";
                    log.accept("P2P group failed reason=" + reason + " (0 error, 1 unsupported, 2 busy)");
                }
            });
        } catch (RuntimeException e) {
            hostFailedAt = System.currentTimeMillis();
            state = "failed (P2P " + e.getClass().getSimpleName() + ")";
            log.accept("P2P group: " + e);
        }
    }

    /** 2.4 GHz band, channel 6 (the middle social channel; every ESP scans all channels anyway). */
    private static android.util.SparseIntArray channels24() {
        android.util.SparseIntArray c = new android.util.SparseIntArray();
        c.put(SoftApConfiguration.BAND_2GHZ, 6);
        return c;
    }

    public void stopFlashNetwork() {
        if (p2pGroup && p2p != null && p2pChannel != null) {
            p2p.removeGroup(p2pChannel, null);
            p2pGroup = false;
        }
        if (reservation != null) reservation.close();
        reservation = null;
        if (lock.isHeld()) lock.release();
        state = "off";
    }

    /** For a single broken device: join its open ESP-RESCUE network directly. */
    public void joinRescue() { joinRescue(RESCUE_SSID); }

    public boolean joined() { return rescueCallback != null && state.startsWith("joined"); }
    /** When hosting the flasher network last failed (0 = never), so callers can fall back. */
    public long hostFailedAt() { return hostFailedAt; }
    public boolean hostingOrStarting() { return reservation != null || p2pGroup || state.equals("starting"); }
    public boolean hosting() { return reservation != null || p2pGroup; }

    public void joinRescue(String ssid) {
        leaveRescue();
        lock.acquire();
        WifiNetworkSpecifier spec = new WifiNetworkSpecifier.Builder().setSsid(ssid)
                .setWpa2Passphrase(PASSPHRASE).build();
        NetworkRequest req = new NetworkRequest.Builder()
                .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
                .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                .setNetworkSpecifier(spec).build();
        rescueCallback = new ConnectivityManager.NetworkCallback() {
            @Override public void onAvailable(Network n) { cm.bindProcessToNetwork(n); log.accept("joined " + ssid); state = "joined " + ssid; }
            @Override public void onUnavailable() { log.accept("could not join " + ssid); state = "off"; }
            @Override public void onLost(Network n) { cm.bindProcessToNetwork(null); log.accept("left " + ssid); state = "off"; }
        };
        state = "joining " + ssid;
        // The system scans for the SSID itself, so no location permission is needed. The
        // timeout lets a rescuer retry when no device is in trouble nearby.
        cm.requestNetwork(req, rescueCallback, JOIN_TIMEOUT_MS);
    }

    public void leaveRescue() {
        if (rescueCallback != null) {
            try { cm.unregisterNetworkCallback(rescueCallback); } catch (RuntimeException ignored) { }
            rescueCallback = null;
        }
        cm.bindProcessToNetwork(null);
    }

    public void stopAll() { stopFlashNetwork(); leaveRescue(); }
}

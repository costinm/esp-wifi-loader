package com.github.costinm.espwifiloader;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.pm.ServiceInfo;
import android.net.wifi.WifiManager;
import android.os.BatteryManager;
import android.os.Binder;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;
import android.util.Log;

import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.ConcurrentLinkedDeque;

/**
 * Owns discovery, the Wi-Fi network and the uploads, so flashing survives the activity.
 *
 * It is a foreground service (with a partial wake lock and a Wi-Fi lock) while
 * <ul>
 * <li>an upload is running,</li>
 * <li>a network it started or joined is held, or</li>
 * <li>the "rescuer" mode is on and the phone is charging: it then also looks for
 *   ESP-RESCUE networks and flashes devices that announce, unattended. Never on battery.</li>
 * </ul>
 * and stops itself otherwise.
 */
public final class FlashService extends Service {
    static final String TAG = "wl-flasher";
    static final String EXTRA_CMD = "cmd";
    static final String EXTRA_BUNDLED = "bundled";
    static final String CMD_FLASH_ALL = "flash-all";
    static final String CMD_HOTSPOT = "hotspot", CMD_RESCUE = "rescue", CMD_STOP = "stop", CMD_START = "start";
    static final String PREFS = "wl", PREF_TEMPLATE = "template", PREF_RESCUER = "rescuer";
    private static final String CHANNEL = "flasher";
    private static final int NOTIFICATION = 1;
    private static final long TICK_MS = 3_000, SCAN_MS = 30_000, RETRY_MS = 10 * 60_000, IDLE_MS = 20_000, HOST_RETRY_MS = 5 * 60_000;

    public final class LocalBinder extends Binder { FlashService service() { return FlashService.this; } }

    private final IBinder binder = new LocalBinder();
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final ConcurrentLinkedDeque<String> logLines = new ConcurrentLinkedDeque<>();
    private final Map<String, Long> attempted = new HashMap<>();
    private final java.util.Set<String> loggedSources = java.util.concurrent.ConcurrentHashMap.newKeySet();
    private Flasher flasher;
    private Discovery discovery;
    private Hotspot hotspot;
    private PowerManager.WakeLock wake;
    private WifiManager.WifiLock wifiLock;
    private boolean foreground;
    /** The network was started by the rescuer, so unplugging the phone releases it. */
    private boolean rescuerOwnsNetwork;
    private long idleSince = System.currentTimeMillis(), lastScan;
    private volatile WlPackage localPackage;

    private final BroadcastReceiver power = new BroadcastReceiver() {
        @Override public void onReceive(Context c, Intent i) { tick(); }
    };

    private void log(String s) {
        Log.i(TAG, s);
        logLines.addLast(s);
        while (logLines.size() > 200) logLines.pollFirst();
    }

    // ---- API for the activity (same process) ----

    public Flasher flasher() { return flasher; }
    public Hotspot hotspot() { return hotspot; }
    public java.util.List<String> logs() { return new java.util.ArrayList<>(logLines); }

    public void setLocalPackage(WlPackage p) { localPackage = p; flasher.localPackage = p; }

    public void flash(String mac, boolean dry) {
        flasher.flash(mac, dry);
        idleSince = System.currentTimeMillis();
        updateForeground();
    }

    /** True while the phone is plugged in. */
    public static boolean isPowered(Context c) {
        Intent b = c.registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
        return b != null && b.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) != 0;
    }

    public boolean rescuerActive() {
        return prefs().getBoolean(PREF_RESCUER, false) && isPowered(this);
    }

    private SharedPreferences prefs() { return getSharedPreferences(PREFS, MODE_PRIVATE); }

    // ---- lifecycle ----

    @Override public IBinder onBind(Intent i) { return binder; }

    @Override public void onCreate() {
        super.onCreate();
        flasher = new Flasher(this::log);
        flasher.setTemplate(prefs().getString(PREF_TEMPLATE, ""));
        // Packages, first match wins: the app's files directories (adb push, see
        // scripts/android-push-packages.sh), then the assets bundled in the APK.
        flasher.assets = name -> {
            java.util.List<java.io.File> dirs = new java.util.ArrayList<>();
            java.io.File ext = getExternalFilesDir("packages");
            if (ext != null) dirs.add(ext);
            dirs.add(new java.io.File(getFilesDir(), "packages"));
            for (java.io.File dir : dirs) {
                java.io.File f = new java.io.File(dir, name);
                if (f.isFile()) {
                    try (java.io.InputStream in = new java.io.FileInputStream(f)) {
                        byte[] b = Http.readCapped(in, 8 << 20);
                        if (!loggedSources.contains(f.getPath())) { loggedSources.add(f.getPath()); log("package " + name + " from " + f.getParent()); }
                        return b;
                    } catch (java.io.IOException e) {
                        log("cannot read " + f + ": " + e.getMessage());
                    }
                }
            }
            try (java.io.InputStream in = getAssets().open("packages/" + name)) {
                byte[] b = Http.readCapped(in, 8 << 20);
                if (!loggedSources.contains("asset:" + name)) { loggedSources.add("asset:" + name); log("package " + name + " from the APK assets"); }
                return b;
            } catch (java.io.IOException e) {
                return null;
            }
        };
        hotspot = new Hotspot(this, this::log);
        discovery = new Discovery(flasher::onAnnounce);
        NotificationChannel ch = new NotificationChannel(CHANNEL, "Flashing", NotificationManager.IMPORTANCE_LOW);
        getSystemService(NotificationManager.class).createNotificationChannel(ch);
        IntentFilter f = new IntentFilter(Intent.ACTION_POWER_CONNECTED);
        f.addAction(Intent.ACTION_POWER_DISCONNECTED);
        registerReceiver(power, f);
        discovery.start();
    }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        // startForegroundService() requires startForeground() promptly, whatever the command.
        updateForeground();
        String cmd = intent == null ? CMD_START : intent.getStringExtra(EXTRA_CMD);
        if (cmd == null) cmd = CMD_START;
        switch (cmd) {
            case CMD_HOTSPOT: hotspot.startFlashNetwork(); break;
            case CMD_RESCUE: // test hook: --es ssid X joins another WPA2 network with the same key
                String ssid = intent.getStringExtra("ssid");
                if (ssid == null) hotspot.joinRescue(); else hotspot.joinRescue(ssid);
                break;
            case "send": sendTo(intent); break;
            case "p2p": { String n = intent.getStringExtra("ssid"); if (n == null) hotspot.startP2pGroup(); else hotspot.startP2pGroup(n); break; }
            case CMD_STOP: hotspot.stopAll(); break;
            case CMD_FLASH_ALL: // test hook: flash every device seen now
                String kind = intent.getStringExtra(EXTRA_BUNDLED);
                if (kind != null) flasher.bundledKind = kind.equals("auto") ? "" : kind;
                for (Flasher.DeviceInfo d : flasher.devices()) flasher.flash(d.announce.mac(), false);
                break;
            default: break;
        }
        idleSince = System.currentTimeMillis();
        handler.removeCallbacks(ticker);
        handler.post(ticker);
        return START_NOT_STICKY;
    }

    private final Runnable ticker = new Runnable() {
        @Override public void run() { tick(); handler.postDelayed(this, TICK_MS); }
    };

    /**
     * Test hook: send a bundled package to a known address (a Main that does not announce
     * yet). adb: --es action send --es host fe80::226e:f1ff:fe13:b170 --es iface wlan0
     * --es chip esp32c6 --es bundled loader [--ez dry true | --ez reboot true]
     */
    private void sendTo(Intent intent) {
        String host = intent.getStringExtra("host"), iface = intent.getStringExtra("iface");
        String chip = intent.getStringExtra("chip"), kind = intent.getStringExtra("bundled");
        boolean dry = intent.getBooleanExtra("dry", false);
        Thread t = new Thread(() -> {
            try {
                java.net.InetAddress raw = java.net.InetAddress.getByName(host);
                java.net.InetAddress addr = java.net.Inet6Address.getByAddress(null, raw.getAddress(),
                        java.net.NetworkInterface.getByName(iface == null ? "wlan0" : iface));
                boolean reboot = intent.getBooleanExtra("reboot", false);
                byte[] data = null;
                if (!reboot) {
                    byte[] bytes = flasher.assets.apply(chip + "-" + kind + ".wlpkg");
                    if (bytes == null) throw new IllegalStateException("no bundled " + chip + "-" + kind);
                    WlPackage pkg = WlPackage.parse(bytes);
                    String bad = pkg.verify();
                    if (bad != null) throw new IllegalStateException("package invalid: " + bad);
                    data = pkg.bytes;
                }
                log("send " + (reboot ? "reboot" : chip + "-" + kind) + " to " + addr);
                UploadResult r = Uploader.upload(new java.net.InetSocketAddress(addr, Wl.PORT), data,
                        dry, reboot, 30_000, line -> { if (!line.startsWith("P ")) log("device: " + line); });
                log("send result: " + r.line);
            } catch (Exception e) {
                log("send failed: " + e);
            }
        }, "wl-send");
        t.setDaemon(true);
        t.start();
        idleSince = System.currentTimeMillis();
    }

    private boolean networkHeld() { return hotspot.hostingOrStarting() || hotspot.joined() || hotspot.state().startsWith("joining"); }

    private void tick() {
        boolean rescuer = rescuerActive();
        long now = System.currentTimeMillis();
        if (rescuer) {
            // The rescuer is the access point: host the flasher network and let devices in trouble
            // come to the phone (they scan for it, join and announce). Where the phone
            // refuses to host (most do, see debugging.md) go to a device's ESP-RESCUE
            // instead, and try hosting again every few minutes.
            if (!networkHeld() && now - lastScan > SCAN_MS) {
                lastScan = now;
                rescuerOwnsNetwork = true;
                long failed = hotspot.hostFailedAt();
                if (failed == 0 || now - failed > HOST_RETRY_MS) {
                    log("rescuer: hosting " + hotspot.flashSsid());
                    hotspot.startFlashNetwork();
                } else {
                    hotspot.joinRescue();
                }
            }
            for (Flasher.DeviceInfo d : flasher.devices()) {
                String mac = d.announce.mac();
                Long last = attempted.get(mac);
                if (d.announce.mode().equals("loader") && now - d.seen < 15_000
                        && (last == null || now - last > RETRY_MS) && !flasher.isBusy(mac)) {
                    attempted.put(mac, now);
                    log("rescuer: flashing " + mac);
                    flasher.flash(mac, false);
                }
            }
        }
        if (!rescuer && rescuerOwnsNetwork && flasher.busyCount() == 0) {
            log("rescuer: not charging, leaving the network");
            hotspot.stopAll();
            rescuerOwnsNetwork = false;
        }
        if (rescuer || flasher.busyCount() > 0 || networkHeld()) idleSince = now;
        updateForeground();
        if (now - idleSince > IDLE_MS) shutdown();
    }

    private void shutdown() {
        handler.removeCallbacks(ticker);
        hotspot.stopAll();
        releaseLocks();
        if (foreground) stopForeground(STOP_FOREGROUND_REMOVE);
        foreground = false;
        stopSelf();
    }

    @Override public void onDestroy() {
        handler.removeCallbacks(ticker);
        discovery.stop();
        hotspot.stopAll();
        releaseLocks();
        try { unregisterReceiver(power); } catch (IllegalArgumentException ignored) { }
        super.onDestroy();
    }

    // ---- foreground + locks ----

    private void acquireLocks() {
        if (wake == null) {
            wake = getSystemService(PowerManager.class).newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "wl:flasher");
            wake.setReferenceCounted(false);
        }
        if (!wake.isHeld()) wake.acquire();
        if (wifiLock == null) {
            wifiLock = ((WifiManager) getApplicationContext().getSystemService(WIFI_SERVICE))
                    .createWifiLock(WifiManager.WIFI_MODE_FULL_LOW_LATENCY, "wl:flasher");
            wifiLock.setReferenceCounted(false);
        }
        if (!wifiLock.isHeld()) wifiLock.acquire();
    }

    private void releaseLocks() {
        if (wake != null && wake.isHeld()) wake.release();
        if (wifiLock != null && wifiLock.isHeld()) wifiLock.release();
    }

    private String summary() {
        int busy = flasher.busyCount();
        if (busy > 0) return "Flashing " + busy + " device" + (busy > 1 ? "s" : "");
        if (rescuerActive()) return "Rescuer active (charging, " + (hotspot.hosting() ? "hosting " + hotspot.flashSsid() : "joining a device") + "), " + flasher.devices().size() + " device(s) seen";
        return "Network: " + hotspot.state();
    }

    private void updateForeground() {
        acquireLocks();
        Notification n = new Notification.Builder(this, CHANNEL)
                .setSmallIcon(android.R.drawable.stat_notify_sync)
                .setContentTitle("ESP Wi-Fi Flasher")
                .setContentText(summary())
                .setContentIntent(android.app.PendingIntent.getActivity(this, 0,
                        new Intent(this, MainActivity.class), android.app.PendingIntent.FLAG_IMMUTABLE))
                .setOngoing(true).build();
        if (!foreground) {
            startForeground(NOTIFICATION, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE);
            foreground = true;
        } else {
            getSystemService(NotificationManager.class).notify(NOTIFICATION, n);
        }
    }
}

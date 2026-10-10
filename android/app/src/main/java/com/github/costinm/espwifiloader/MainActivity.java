package com.github.costinm.espwifiloader;

import android.Manifest;
import android.app.Activity;
import android.content.BroadcastReceiver;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.ServiceConnection;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.text.InputType;
import android.view.View;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Spinner;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.InputStream;
import java.util.ArrayList;
import java.util.List;

/**
 * Small widget UI; all work happens in {@link FlashService}.
 *
 * Brand-new device: type the domain (or URL) to download the app from. A device that has
 * installed Main announces its own domain. Tap a device to flash it, long-press for a dry run.
 */
public final class MainActivity extends Activity {
    private static final int PICK = 2;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private FlashService service;
    private TextView status, packageInfo, logView;
    private EditText source;
    private CheckBox rescuer;
    private ArrayAdapter<String> adapter;
    private final List<String> macs = new ArrayList<>();
    private SharedPreferences prefs;
    private String kindValue = "";

    private final ServiceConnection conn = new ServiceConnection() {
        @Override public void onServiceConnected(ComponentName n, IBinder b) {
            service = ((FlashService.LocalBinder) b).service();
            service.flasher().setTemplate(source.getText().toString());
            service.flasher().bundledKind = kindValue;
        }
        @Override public void onServiceDisconnected(ComponentName n) { service = null; }
    };

    private final BroadcastReceiver power = new BroadcastReceiver() {
        @Override public void onReceive(Context c, Intent i) { applyRescuer(); }
    };

    private final Runnable refresh = new Runnable() {
        @Override public void run() { render(); ui.postDelayed(this, 1000); }
    };

    private static LinearLayout.LayoutParams match() {
        return new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
    }

    private Button button(String text, View.OnClickListener l) {
        Button b = new Button(this);
        b.setText(text);
        b.setAllCaps(false);
        b.setOnClickListener(l);
        b.setLayoutParams(new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        return b;
    }

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        prefs = getSharedPreferences(FlashService.PREFS, MODE_PRIVATE);
        int pad = (int) (12 * getResources().getDisplayMetrics().density);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(pad, pad, pad, pad);

        status = new TextView(this);
        root.addView(status, match());

        source = new EditText(this);
        source.setHint("Download for new devices: domain or https://host/{chip}.wlpkg");
        source.setInputType(InputType.TYPE_TEXT_VARIATION_URI);
        source.setSingleLine();
        source.setText(prefs.getString(FlashService.PREF_TEMPLATE, ""));
        root.addView(source, match());

        packageInfo = new TextView(this);
        root.addView(packageInfo, match());

        rescuer = new CheckBox(this);
        rescuer.setText("Rescuer: act as the access point (flash network) and flash devices while charging");
        rescuer.setChecked(prefs.getBoolean(FlashService.PREF_RESCUER, false));
        rescuer.setOnCheckedChangeListener((v, on) -> { prefs.edit().putBoolean(FlashService.PREF_RESCUER, on).apply(); applyRescuer(); });
        root.addView(rescuer, match());

        LinearLayout row = new LinearLayout(this);
        row.addView(button("Join ESP-RESCUE", v -> command(FlashService.CMD_RESCUE)));
        row.addView(button("Host flash network", v -> command(FlashService.CMD_HOTSPOT)));
        root.addView(row, match());
        LinearLayout row2 = new LinearLayout(this);
        row2.addView(button("Choose file…", v -> startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT)
                .addCategory(Intent.CATEGORY_OPENABLE).setType("*/*"), PICK)));
        row2.addView(button("Stop network", v -> command(FlashService.CMD_STOP)));
        root.addView(row2, match());

        Spinner kind = new Spinner(this);
        String[] kinds = {"Package: auto (file, domain, announce)", "Bundled: main (bootstrap build)",
                "Bundled: loader", "Bundled: stage2 4 MB", "Bundled: stage2 8 MB",
                "Bundled: reboot into loader"};
        String[] values = {"", "main", "loader", "stage2-4mb", "stage2-8mb", "request-loader"};
        kind.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_spinner_dropdown_item, kinds));
        kind.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(AdapterView<?> p, View v, int pos, long id) {
                kindValue = values[pos];
                if (service != null) service.flasher().bundledKind = kindValue;
            }
            @Override public void onNothingSelected(AdapterView<?> p) { }
        });
        root.addView(kind, match());

        ListView list = new ListView(this);
        adapter = new ArrayAdapter<>(this, android.R.layout.simple_list_item_1, new ArrayList<>());
        list.setAdapter(adapter);
        list.setOnItemClickListener((p, v, pos, id) -> flash(pos, false));
        list.setOnItemLongClickListener((p, v, pos, id) -> { flash(pos, true); return true; });
        root.addView(list, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1));

        logView = new TextView(this);
        logView.setTextSize(11);
        ScrollView scroll = new ScrollView(this);
        scroll.addView(logView);
        root.addView(scroll, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1));
        setContentView(root);

        String[] want = {Manifest.permission.NEARBY_WIFI_DEVICES, Manifest.permission.POST_NOTIFICATIONS};
        List<String> missing = new ArrayList<>();
        for (String p : want) if (checkSelfPermission(p) != PackageManager.PERMISSION_GRANTED) missing.add(p);
        if (!missing.isEmpty()) requestPermissions(missing.toArray(new String[0]), 1);

        // Bind first; commands start the service as a foreground service.
        startService(new Intent(this, FlashService.class).putExtra(FlashService.EXTRA_CMD, FlashService.CMD_START));
        bindService(new Intent(this, FlashService.class), conn, 0);
        handleTestAction(getIntent());
    }

    @Override protected void onStart() {
        super.onStart();
        IntentFilter f = new IntentFilter(Intent.ACTION_POWER_CONNECTED);
        f.addAction(Intent.ACTION_POWER_DISCONNECTED);
        registerReceiver(power, f);
        ui.post(refresh);
    }

    @Override protected void onStop() {
        ui.removeCallbacks(refresh);
        unregisterReceiver(power);
        prefs.edit().putString(FlashService.PREF_TEMPLATE, source.getText().toString().trim()).apply();
        super.onStop();
    }

    @Override protected void onDestroy() {
        unbindService(conn);
        super.onDestroy();
    }

    @Override protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        handleTestAction(intent);
    }

    /** adb test hook: am start -n .../.MainActivity --es action hotspot|rescue|stop (scripts/android-test.sh). */
    private void handleTestAction(Intent intent) {
        String action = intent.getStringExtra("action");
        if (action == null) return;
        // adb extras (bundled, ssid, host, iface, chip) go to the service as they are.
        Intent i = new Intent(this, FlashService.class);
        if (intent.getExtras() != null) i.putExtras(intent.getExtras());
        i.putExtra(FlashService.EXTRA_CMD, action);
        startForegroundService(i);
    }

    private void command(String cmd) {
        prefs.edit().putString(FlashService.PREF_TEMPLATE, source.getText().toString().trim()).apply();
        if (service != null) service.flasher().setTemplate(source.getText().toString());
        startForegroundService(new Intent(this, FlashService.class).putExtra(FlashService.EXTRA_CMD, cmd));
    }

    /** Keep the service up while the rescuer is on and the phone charges. */
    private void applyRescuer() {
        if (rescuer.isChecked() && FlashService.isPowered(this)) command(FlashService.CMD_START);
    }

    private void flash(int pos, boolean dry) {
        if (service == null || pos >= macs.size()) return;
        service.flasher().setTemplate(source.getText().toString());
        command(FlashService.CMD_START); // foreground first: the upload may outlive this screen
        service.flash(macs.get(pos), dry);
    }

    private void render() {
        if (service == null) { status.setText("starting…"); return; }
        status.setText("Network: " + service.hotspot().state() + "\nPower: "
                + (FlashService.isPowered(this) ? "charging" : "battery (rescuer is off)"));
        packageInfo.setText(service.flasher().localPackage == null ? "" : "Using file: "
                + service.flasher().localPackage.header.fqdn + " v" + service.flasher().localPackage.header.version);
        List<Flasher.DeviceInfo> list = new ArrayList<>(service.flasher().devices());
        list.sort((x, y) -> x.announce.mac().compareTo(y.announce.mac()));
        macs.clear();
        adapter.clear();
        for (Flasher.DeviceInfo d : list) {
            Announce a = d.announce;
            macs.add(a.mac());
            adapter.add(a.chip() + " " + a.mac() + "\n" + a.mode() + " " + a.state() + " v" + a.version()
                    + " " + a.role() + " - " + (d.status.isEmpty() ? "tap to flash" : d.status));
        }
        adapter.notifyDataSetChanged();
        logView.setText(String.join("\n", service.logs()));
    }

    @SuppressWarnings("deprecation")
    @Override protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode != PICK || resultCode != RESULT_OK || data == null || data.getData() == null || service == null) return;
        Uri uri = data.getData();
        try (InputStream in = getContentResolver().openInputStream(uri)) {
            WlPackage pkg = WlPackage.parse(Http.readCapped(in, 8 << 20));
            String bad = pkg.verify();
            if (bad != null) { service.flasher().localPackage = null; packageInfo.setText("Rejected: " + bad); return; }
            service.setLocalPackage(pkg);
        } catch (Exception e) {
            packageInfo.setText("File error: " + e.getMessage());
        }
    }
}

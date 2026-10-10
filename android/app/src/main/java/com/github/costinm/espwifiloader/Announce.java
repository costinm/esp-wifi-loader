package com.github.costinm.espwifiloader;

import java.util.HashMap;
import java.util.Map;

/** One `esp-wifi-loader ...` multicast line. */
public final class Announce {
    public final Map<String, String> fields;

    private Announce(Map<String, String> fields) { this.fields = fields; }

    /** Null when the line is not an announce. */
    public static Announce parse(String line) {
        String[] parts = line.trim().split("\\s+");
        if (parts.length == 0 || !parts[0].equals("esp-wifi-loader")) return null;
        Map<String, String> m = new HashMap<>();
        for (int i = 1; i < parts.length; i++) {
            int eq = parts[i].indexOf('=');
            if (eq > 0) m.put(parts[i].substring(0, eq), parts[i].substring(eq + 1));
        }
        return new Announce(m);
    }

    private String get(String k, String dflt) { String v = fields.get(k); return v == null ? dflt : v; }

    public String mode() { return get("mode", "loader"); }
    public String state() { return get("state", "?"); }
    public String chip() { return get("chip", "?"); }
    public String mac() { return get("mac", "?"); }
    public String role() { return get("role", "?"); }
    public String key() { return get("key", "none"); }
    /** The loader key in the device's Stage2 (signs the loader and Stage2); "none" without one. */
    public String anchor() { return get("anchor", "none"); }
    public String fqdn() { return get("fqdn", ""); }
    public long version() { try { return Long.parseLong(get("version", "0")); } catch (NumberFormatException e) { return 0; } }
    public int port() { try { return Integer.parseInt(get("port", "")); } catch (NumberFormatException e) { return Wl.PORT; } }
}

package com.github.costinm.espwifiloader;

import java.net.DatagramPacket;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.MulticastSocket;
import java.net.NetworkInterface;
import java.net.SocketTimeoutException;
import java.nio.charset.StandardCharsets;
import java.util.Collections;
import java.util.function.BiConsumer;

/** Joins ff02::5227 on every multicast-capable interface and reports announces. */
public final class Discovery {
    private final BiConsumer<Announce, InetAddress> onAnnounce;
    private volatile boolean running;
    private volatile MulticastSocket socket;

    public Discovery(BiConsumer<Announce, InetAddress> onAnnounce) { this.onAnnounce = onAnnounce; }

    public void start() {
        if (running) return;
        running = true;
        Thread t = new Thread(this::loop, "wl-discovery");
        t.setDaemon(true);
        t.start();
    }

    public void stop() {
        running = false;
        MulticastSocket s = socket;
        if (s != null) s.close();
    }

    private void loop() {
        while (running) {
            try (MulticastSocket s = new MulticastSocket(Wl.PORT)) {
                socket = s;
                s.setSoTimeout(1000);
                InetSocketAddress group = new InetSocketAddress(InetAddress.getByName(Wl.GROUP), 0);
                for (NetworkInterface ni : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                    if (ni.isUp() && ni.supportsMulticast() && !ni.isLoopback()) {
                        try { s.joinGroup(group, ni); } catch (java.io.IOException ignored) { }
                    }
                }
                // Rejoin periodically: a hotspot or joined network can appear later.
                long rejoin = System.currentTimeMillis() + 10_000;
                byte[] buf = new byte[1500];
                while (running && System.currentTimeMillis() < rejoin) {
                    DatagramPacket p = new DatagramPacket(buf, buf.length);
                    try {
                        s.receive(p);
                    } catch (SocketTimeoutException e) {
                        continue;
                    }
                    Announce a = Announce.parse(new String(p.getData(), 0, p.getLength(), StandardCharsets.UTF_8));
                    if (a != null) onAnnounce.accept(a, p.getAddress());
                }
            } catch (Exception e) {
                if (!running) return;
                try { Thread.sleep(1000); } catch (InterruptedException ie) { return; }
            }
        }
    }
}

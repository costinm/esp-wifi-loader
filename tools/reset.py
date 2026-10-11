#!/usr/bin/env python3
"""Reset a board from the computer, including the sequence that starts the loader (docs/boot.md, "Entering the loader").

  reset.py --port PORT reset [--times N] [--gap S]   N resets in a row
  reset.py --port PORT loader                        quick resets (five at most from a clean count): Stage2 starts the loader instead of Main
  reset.py --port PORT recover                       the same five, for a Main that hangs (the loader returns to a valid Main
                                                     by itself after CONFIG_WL_IDLE_TIMEOUT_S)

Stage2 counts resets that follow each other within CONFIG_WL_BOOT_QUICK_WINDOW_TICKS (about five seconds, from the
first) and starts the loader on the CONFIG_WL_BOOT_QUICK_RESETS-th (five). Each reset must come after Stage2 has run far
enough to count it (about 0.3 s after the reset), hence the default gap of 0.7 s between them.

How a reset is made: the USB-Serial/JTAG port of the ESP32-C6 and ESP32-S3 resets the chip when DTR goes low while RTS is
high (the esptool convention), and releasing RTS lets it boot. A plain open of the port, or dropping RTS first, does not
reset it (measured on an ESP32-C6). The tool stops at the first loader banner: resets already counted in the window bring the verdict earlier than the fifth. The chip re-enumerates on the USB bus after a reset; the port is reopened. A board
behind a UART bridge resets the same way when DTR/RTS are wired to EN and GPIO0.

Needs pyserial (it is in the ESP-IDF environment). Prints the console afterwards so the boot can be seen.
"""
import argparse
import sys
import time

QUICK_RESETS = 5          # CONFIG_WL_BOOT_QUICK_RESETS


class Port:
    def __init__(self, path, serial_factory=None, sleep=time.sleep):
        self.path, self._sleep = path, sleep
        self._open = serial_factory or self._open_serial
        self.ser = None

    @staticmethod
    def _open_serial(path):
        import serial
        return serial.Serial(path, 115200, timeout=0.2)

    def open(self, wait=10.0):
        end = time.time() + wait
        while True:
            try:
                self.ser = self._open(self.path)
                return self
            except (OSError, ValueError) as e:
                if time.time() > end:
                    raise SystemExit(f"cannot open {self.path}: {e}")
                self._sleep(0.3)

    def close(self):
        if self.ser:
            try:
                self.ser.close()
            finally:
                self.ser = None

    def reset(self):
        """One reset: DTR low with RTS high, then release RTS."""
        self.ser.rts = True
        self.ser.dtr = False
        self._sleep(0.1)
        self.ser.rts = False

    def to_loader(self, gap=0.7, most=8):
        """Reset until Stage2 starts the loader, and stop there: resets already counted in the five second window make
        the verdict come earlier than the fifth, and one more reset would boot Main again. Returns the console text."""
        out = ""
        for i in range(most):
            try:
                self.reset()
            except (OSError, AttributeError):
                self.close()
                self.open()
                self.reset()
            out += self.read(gap, until="loader v1")
            if "loader v1" in out:
                return out
        return out

    def reset_times(self, n, gap=0.7):
        """n resets, `gap` seconds apart; returns the console text of all the boots in between (Stage2 prints its count)."""
        out = ""
        for i in range(n):
            try:
                self.reset()
            except (OSError, AttributeError):          # the port went away with the last reset
                self.close()
                self.open()
                self.reset()
            if i < n - 1:
                out += self.read(gap)
        return out

    def read(self, seconds, until=None):
        out, end = "", time.time() + seconds
        while time.time() < end:
            try:
                out += self.ser.read(400).decode(errors="replace")
            except OSError:
                self.close()
                self.open()
                continue
            if until and until in out:
                break
        return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--port", required=True, help="e.g. /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_...-if00")
    ap.add_argument("cmd", choices=["reset", "loader", "recover"])
    ap.add_argument("--times", type=int, default=1)
    ap.add_argument("--gap", type=float, default=0.7)
    ap.add_argument("--seconds", type=float, default=8.0, help="how long to print the console afterwards")
    a = ap.parse_args(argv)
    p = Port(a.port).open()
    try:
        if a.cmd == "reset":
            out = p.reset_times(a.times, a.gap) + p.read(a.seconds)
        else:
            out = p.to_loader(a.gap)
            out += p.read(a.seconds if "loader v1" not in out else 1.0)
        sys.stdout.write(out)
    finally:
        p.close()
    if a.cmd != "reset":
        entered = "loader v1" in out
        print("\n=> " + ("the loader is running" if entered else "no loader banner: not entered (a longer --gap? is the board running Stage2?)"))
        return 0 if entered else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

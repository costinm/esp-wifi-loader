# esp-wifi-loader

https://github.com/costinm/esp-wifi-loader

A small(er) image that installs your application ("Main") over Wi-Fi, like the Arduino
serial bootloader but over Wi-Fi. Extracted and simplified from costinm/dmesh's Stage2 and Recovery.

Principle: the ESP is low-trust. It should never need the owner's home Wi-Fi credentials
and never joins the home network. Separate 'flash networks' are used; integrity comes from signed
images (ECDSA P-256, key pinned on first install), not from the network. An Android app hosts or
joins the ESP's flash network, gets images from the net or from storage, and acts as the flash station.

| Path | What |
|---|---|
| `boot/` | Stage2: the second-stage bootloader that selects Main or the loader |
| `loader/` | the loader: joins or hosts a flash network and installs signed images |
| `bootstrap/` | the same code built as Main: installs the loader and Stage2 (docs/bootstrap.md) |
| `components/esp_wifi_loader/` | the shared code; an application can link it |
| `android/` | the Android app: the flash station |
| `tools/` | `sign.py`, `upload.py`: sign images, send them from a computer |
| `web/` | GitHub Pages: install the first image from the browser |
| `docs/` | design: `boot.md`, `protocol.md`, `app-integration.md`, `building.md` |

## Android app

The phone is the flash station. Nothing needs to be typed or configured for a device that is
already set up.

- **Flash a device.** Join the device's rescue network from the app (Android asks once), pick the
  device in the list and tap it. The app takes the image from a file you choose, from the domain the
  device reports, or, for a brand-new board, from a domain you type in, checks that it is signed by the
  right key, and installs it. A full application takes a few seconds.
- **Rescuer mode.** Tick the box and leave a charging phone running: it looks for devices in trouble
  and fixes them by itself. It only runs while the phone is plugged in, never on battery, and keeps
  working with the screen off.
- **Bring up a blank board.** Install the first image from the browser (the web flasher on the project
  page), then everything else goes through the phone. The app can carry the loader images itself, so the
  loader can even be replaced over Wi-Fi.

Android 13 or newer. Phones cannot host the flash network reliably yet, so the ESP hosts it and
the phone joins; see `android/README.md`.

## Using it

- Install a first image: the web flasher (`web/`), or the files from a release with esptool.
- Put your application in the loader's reach: sign it with `tools/sign.py`, then flash it from the
  phone or with `tools/upload.py`. `docs/app-integration.md` shows how an application links the code
  and asks to be upgraded.
- To build the firmware or the app yourself: `docs/building.md`.

## Status

Working on ESP32-C6 with an Android 15 phone: blank board to installed application, the loader and
Stage2 replacing themselves over Wi-Fi, and the unattended rescuer. ESP32 and ESP32-S3 build and are not
yet run on hardware. Hosting the network from the phone does not work on any phone tried. DMesh's
firmware links the code in and builds; flashing through it is still to be tested. Open items are in
`TODO.md`, test notes and findings in `debugging.md`.

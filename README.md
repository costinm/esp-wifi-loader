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
| `src/`, `Cargo.toml` | the same protocol as a Rust crate, for firmware written in Rust (DMesh): wire format, signature check, state log, a one-connection server |
| `tools/` | all the scripting, for the loader and for the applications built on it: `sign.py`, `sign-bundle.py` (sign a tree of images; `Dockerfile.sign`), `upload.py` (send a package to the loader or to an application's upload port, optionally through a phone's netcat), `provision.py` (put an application on a board with esptool and the flasher phone: backup, prepare, flash, verify), `reset.py` (reset a board, or make the five resets that start the loader) |
| `web/` | GitHub Pages: install the first image from the browser |
| `docs/` | design: `boot.md`, `protocol.md`, `app-integration.md`, `building.md` |

Keys: the github secret in this repository signs only the loader and Stage2 built here; an application using the loader signs its
own Main with its own key. Fork it to use your own keys with the same GitHub workflow and app (docs/building.md).

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

Android 13 or newer. The phone can host the flash network itself (a Wi-Fi Direct group, WPA2, 2.4 GHz)
so devices come to it, or join the device's own network; see `android/README.md`.

## Using it

### Sign and flash from Linux

Build unsigned firmware with `scripts/build.sh`. Application inputs use
`target/out/<cpu>/main.bin` (or `main-app.bin`) and optional
`target/out/<cpu>/modules/mod_<name>.dmod`. The batch signer writes
`<cpu>.wlpkg`, `<cpu>-<name>.wlpkg`, and `SHA256SUMS` to one output directory.
It requires an existing P-256 PEM private key and a numeric version. It never
creates a key implicitly.

```sh
podman build -f Dockerfile.sign -t esp-wifi-sign .
rm -rf target/rel && mkdir -p target/rel
podman run --rm -v "$PWD/target/out:/input:ro" \
  -v "$HOME/.ssh/my-app-key.pem:/key/key.pem:ro" -v "$PWD/target/rel:/output" \
  esp-wifi-sign --version 12 --fqdn owner/repo:latest
```

On Nix, `nix profile install path:$PWD#default` installs `esp-wifi-sign`,
`esp-wifi-sign-one`, `esp-wifi-upload`, and `espflash`. Run the signer with
`--input target/out --key /path/to/key.pem --output target/rel --version 12
--fqdn owner/repo:latest`. The uploader runs on a Linux machine bridged or
connected to the flash network: `esp-wifi-upload discover --iface wlan0`, then
`esp-wifi-upload send target/rel/esp32c6.wlpkg --host fe80::ADDR%wlan0`.
An `OK` reply confirms protocol acceptance; check the rebooted application
separately.

`WL_FIRMWARE_ROOT=$PWD/target/out nix build --impure path:$PWD#with-firmware`
packages the already-built unsigned `stage2-*.bin`, `partition-table-*.bin`,
`loader.bin`, `bootstrap.bin`, and composite `factory-*.bin` under
`share/esp-wifi-loader/flash/<cpu>/`. `#firmware` provides just these files.
The private key and signed packages are never inputs to that Nix build.

- Install a first image: the web flasher (`web/`), or the files from a release with esptool.
- Put your application in the loader's reach: sign it with `tools/sign.py`, then flash it from the
  phone or with `tools/upload.py`. `docs/app-integration.md` shows how an application links the code
  and asks to be upgraded.
- To build the firmware or the app yourself: `docs/building.md`.

## Status

Working on ESP32-C6 with an Android 15 phone: blank board to installed application, the loader and
Stage2 replacing themselves over Wi-Fi, and the unattended rescuer, with the phone as the access point.
ESP32 and ESP32-S3 build and are not yet run on hardware. Android 17 phones are still to be retested. DMesh's
firmware links the code in and builds; flashing through it is still to be tested. Open items are in
`TODO.md`, test notes and findings in `debugging.md`.

# ESP Wi-Fi Flasher (Android)

Implements docs/protocol.md from the phone's side. Plain Java, no libraries, old-style
widgets. The protocol code (`Wl`, `WlPackage`, `Announce`, `Uploader`, `Discovery`,
`Flasher`) has no Android classes and is unit tested on the JVM; `FlashService`, `Hotspot`
and `MainActivity` are the Android parts.

## How it works

- **Networks**: both flash networks are WPA2 with the public constant passphrase
  `untrusted-open-mode`. The phone joins a device's `ESP-RESCUE` with `WifiNetworkSpecifier`
  (a system dialog the first time; the system scans, so no location permission), or can try to
  host the flasher network (`startLocalOnlyHotspotWithConfiguration`; the Android 17 Pixels tested
  ignored or refused it, see debugging.md).
- **Where the package comes from**: a file you pick; else the app's files directory
  (`/sdcard/Android/data/com.github.costinm.espwifiloader/files/packages`, or the internal `files/packages`;
  `scripts/android-push-packages.sh` copies there over adb, no new APK needed); else the device's announced source
  (`fqdn`) or, for brand-new devices, the text box. A source is `owner/repo[:tag]` (the assets of that GitHub
  release; tag `latest` by default, so `costinm/dmesh:latest` means
  `https://github.com/costinm/dmesh/releases/download/latest/<chip>.wlpkg`), a bare host (`https://host/fw/<chip>.wlpkg`),
  a host with a path, or a URL (with `{chip}` or `<chip>.wlpkg` appended). A device that runs the bootstrap build asks
  for `<chip>-loader.wlpkg` instead; else the assets bundled in the APK (below). The Spinner can force one bundled package.
  Packages are verified (signature, hashes, chip, key pin, version) before sending.
- **Foreground service** (`FlashService`, type `connectedDevice`, partial wake lock + Wi-Fi
  lock): runs while an upload is in progress or a network is held, then stops itself.
- **Rescuer** checkbox: the phone acts as the access point. While it is charging the service
  stays up, hosts the flasher network (`DIRECT-ef-flash`, WPA2) and devices in trouble scan for it, join
  and announce; it flashes those that announce in loader mode (each at most every 10 minutes).
  Where the phone refuses to host (every phone tested so far, see debugging.md) it falls back to
  joining a device's `ESP-RESCUE`, and retries hosting every 5 minutes. On battery it does not
  run: unplugging leaves the network and stops the service. Plugging back in needs the app to be
  opened (Android delivers no power broadcast to a stopped app). Joining `ESP-RESCUE` by hand
  ("Join ESP-RESCUE") is the manual, one-device path.

## Can the phone host the flash network? Yes, as a Wi-Fi Direct group

An open, freely named hotspot is not available to apps. What works is an ordinary WPA2 access point:

1. **Wi-Fi P2P group** (Android 10+): the phone becomes a group owner named `DIRECT-ef-flash` with the
   public passphrase, on 2.4 GHz (ESP32 radios do not do 5 GHz). The loader looks for this name as well as
   any `DIRECT-xy-flash`. **Verified on a Pixel 3a (Android 15):** group on 2437 MHz, the C6 joined it as a
   station and the rescuer installed a full application over it (4.8 s).
2. **Local-only hotspot with a chosen name** (`startLocalOnlyHotspotWithConfiguration`, Android 17):
   tried first when it exists, with the band forced to 2.4 GHz. Not verified: the two Android 17 phones
   tested earlier ignored the name or refused (`ERROR_INCOMPATIBLE_MODE`, before the band was set to
   2.4 GHz and with DMesh's own app possibly holding a Wi-Fi mode), and were locked for the retest.
3. **Joining the device's `ESP-RESCUE`** (`WifiNetworkSpecifier`, one system prompt): the fallback, and
   the manual path.

The rescuer tries them in that order and retries hosting every few minutes.

## Bundled assets

`scripts/pack-android-assets.sh [KEY.pem]` signs the built images into
`app/src/main/assets/packages/` (git-ignored), per chip: `<chip>-main.wlpkg` (the bootstrap
build, the loader code as Main), `<chip>-loader.wlpkg`, `<chip>-stage2-4mb|8mb.wlpkg`. With these a
phone can bring up a blank board and cycle the loader: the loader installs `main` (the bootstrap
build), the bootstrap build installs `loader` and `stage2`. Without a key argument it makes a dev
key under `target/`; release builds need the real key.

## Build

```sh
cd android
export ANDROID_HOME=...   # SDK with platforms;android-36.1 (API 36.1)
./gradlew :app:testDebugUnitTest :app:assembleDebug     # output under ../target/android-build
```

Tests use `src/test/resources/sample.wlpkg`, a package made by `tools/sign.py` with a
throwaway key. The reuse-DMesh setup works here too: source `/ws/dmesh/env.sh` and
`dmesh-setenv` for JDK and SDK paths (the build is plain Java, so `--offline` works with DMesh's Gradle cache).

## Not done

- Tested on a Pixel 3a (Android 15) only; no hotspot path works yet (debugging.md).
- One ESP-RESCUE network at a time; no resume after a lost link; no package index.
- No notification action to stop the rescuer.

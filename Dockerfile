# Build images for esp-wifi-loader (podman or docker).
#
# The official Espressif image already has ESP-IDF v6.0.2 and the esp32 /
# esp32s3 / esp32c6 toolchains, so nothing is downloaded at build time.
#
#   podman build --target build-env -t esp-wifi-loader-build .
#   podman run --rm -v "$PWD":/src:Z -w /src esp-wifi-loader-build scripts/build.sh esp32c6 4mb
#
# Only the binaries, into ./target/out:
#   podman build --target bin --output type=local,dest=target/out .
#
# Android SDK image (for the future flashing app; ships SDK 36 + NDK):
#   podman build --target android-env -t esp-wifi-loader-android .
ARG IDF_IMAGE=docker.io/espressif/idf:v6.0.2
ARG ANDROID_IMAGE=ghcr.io/cirruslabs/android-sdk:36-ndk

FROM ${IDF_IMAGE} AS build-env
# openssl: tools/sign.py fallback and the host tests.
RUN apt-get update && apt-get install -y --no-install-recommends openssl \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
# The image entrypoint sources ESP-IDF's export.sh, then runs the command.

FROM build-env AS build
COPY . /src
RUN scripts/build.sh all all

FROM scratch AS bin
COPY --from=build /src/target/out/ /

FROM ${ANDROID_IMAGE} AS android-env
WORKDIR /src

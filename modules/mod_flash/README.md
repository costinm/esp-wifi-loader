# Signed object flash module

This implements the verification and flash protocols as a `esp-xip-modules` 
module. 

It is `no_std` and depends on `espmod-abi`, `sha2` and `p256`. It does not
link ESP-IDF, FreeRTOS or the XIP host implementation.

Build with the checked-in xip-modules harness:

```sh
. ./env.sh
../esp-xip-modules/scripts/build-mod.sh modules/mod_flash --chip esp32c6 \
  --slots ../esp-xip-modules/crates/espmod-host/src/loader.rs

# Use --chip esp32 or --chip esp32s3 for the other targets.

scripts/test-host.sh rust
```

TODO: remove '--slots' - use an ID if we have to, loader should not hardcode or know about them
TODO: esp-xip-modules flake should install to $HOME/opt/exp-xip-modules and have the script and other artifacts there. A docker image for esp-xip-modules is option 2 - no direct use of the repo. Document the exact command to get the flake and build with it.
TODO: change the script to produce output under top level target, leave the rest of git clean

Build outputs are `modules/mod_flash/target/out/<cpu>/modules/mod_flash.{dmod,elf}`
(or the directory passed with `--out`). 

Install the signed module with the standalone loader or esptool. The existing `tools/sign.py`
and `tools/upload.py` work with it.

Start it with the existing module-start operation (`start flash` on a compatible console).
It listens on TCP port 5227; stopping it closes the listener, connection, packet
handles and all transfer allocations. 

TODO: evolve the TCP protocol to do its own dispatching to modules, so we share a listener (in host).

The xip-modules ABI runtime provides the required HAL and networking.

The buffers are allocated only during an upload and all memory must be freed on stop.

## Installation sequence

- One connection at a time; a second is closed. Reads go directly into a pool
  packet. Arbitrary TCP fragmentation and partial writes are supported.
  
- The shared `Session` checks signature, pinned key, minimum version, CPU,
  target, sizes, block table, every block hash, whole-image hash and terminal
  FIN. Both image buffers are allocated before an erase is requested.

- Module targets use the build's slot table. The selected target module is
  stopped before its bytes are erased. The flasher cannot replace itself.
  Loader/Stage2 updates stop other XIP modules. The control plane also quiesces
  Main's other services and flash users before starting such an upgrade.

- Flash writes transfer pool packets to asynchronous HAL jobs. There is no
  extra host block buffer. Body blocks are written first; block zero stays in
  the session until durable state-log writes complete.

- The module reads the loader's existing two-sector state log on each new
  connection. A small journal plans appends/compaction, then executes them in
  order through the flash ABI. A failed commit never writes block zero and
  never produces `OK`.

- Loader/recovery and Stage2 image updates restart after submitting the reply.
  Main image uploads record the signed header and Main minimum version before
  writing block zero, then restart. A signed zero-length Main request retains
  the existing RTC loader handoff.
  
TODO: main will use an authenticated request to start the self-upgrade, the zero-length
remains a short term workaround until auth is added to xip-modules


## Signed raw flash

`target=5` uses the existing signed name field as a hexadecimal physical flash
address. The signer normalizes it to eight digits:

```sh
tools/sign.py sign --key OWNER.pem --cpu esp32c6 --version 1 \
  --target flash --address 0x9000 --image data.bin --out data.wlpkg
```

CPU, pinned (TOFU or provisioned) owner signature, block hashes, whole-image hash 
and FIN are checked, as described in protocol.md 

## Implementation notes

This target has no per-address rollback record and leaves the existing Main/module version records unchanged.

For internal debugg, the module allows unaligned address - but erases entire 
blocks.

There is no partition restriction or ESP/DMOD image-header check. Raw uploads
do not restart automatically. Named targets 1–4 retain their existing meanings
and state records. The owner/control plane must coordinate active mappings and
other flash users, including any write to this module or its running host.

Dry runs verify without erasing, stopping modules or updating state - may be removed.



TODO: we should add back the per-address rollback and support only 4k aligned in the sign.py - the flasher may have the ability to flash at any location and in future write RAM or PRAM, but no 
need to expose this to normal users, outside of debugging.

TODO: fix the versioning, add a bit to manifest to request a reboot after.
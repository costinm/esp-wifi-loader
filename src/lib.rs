//! The esp-wifi-loader upload protocol (docs/protocol.md) for Rust firmware.
//!
//! * [`wire`]: transport header, signed header, package layout, signature check.
//! * [`state`]: the two-sector append-only state log (pinned key, minimum versions) in the last
//!   8 KiB of the loader partition, byte-compatible with the C loader's.
//! * [`device`] (feature `std`): serves one TCP connection: verifies and installs a signed
//!   package through a [`device::Platform`], or answers a zero-length Main manifest.
//!
//! The crate does no I/O of its own beyond the stream it is given. The firmware supplies the flash
//! (partition sinks, state sectors) and the accept loop.
#![cfg_attr(not(feature = "std"), no_std)]

extern crate alloc;

#[cfg(feature = "std")]
pub mod device;
pub mod rtc;
pub mod state;
pub mod wire;

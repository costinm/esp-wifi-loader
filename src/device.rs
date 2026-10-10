//! Serving one connection on a device: verify and install a signed package, or answer a zero-length
//! Main manifest. The firmware provides the stream (an accepted TCP socket with timeouts), the
//! flash destinations ([`Platform`]) and the state sectors ([`FlashOps`]); this module is the same
//! sequence of checks as the C loader's `upload.c`:
//!
//! 1. before anything is erased: signature, pinned key, minimum version, chip, target, sizes, and the
//!    block table against its hash;
//! 2. per 4 KiB block: its hash against the table, then written (block 0 is held back);
//! 3. the whole-image hash, then the client's FIN;
//! 4. commit: block 0 last, then the state records. An interrupted install leaves block 0 unwritten, so
//!    the image cannot boot.

use std::io::{Read, Write};
use std::time::Instant;

use sha2::{Digest, Sha256};

use crate::state::{FlashOps, StateLog, REC_MINVER, REC_MINVER_LOADER, REC_MINVER_STAGE2, REC_PIN};
use crate::wire::{self, target, Header, Reject, Transport, BLOCK, HEADER_SIZE, PUBKEY_SIZE, SIG_SIZE, TABLE_ENTRY};

/// Where one image goes.
pub trait Sink {
    /// Once, before the first block, with the image length.
    fn begin(&mut self, len: u32) -> Result<(), ()>;
    /// A 4 KiB block (the last may be shorter). Blocks arrive in order except block 0, which is
    /// written last.
    fn write_block(&mut self, index: u32, bytes: &[u8]) -> Result<(), ()>;
    fn finish(&mut self) -> Result<(), ()>;
}

pub trait Platform {
    type Sink: Sink;
    /// `esp_chip_id_t` of the running chip ([`wire::cpu`]).
    fn cpu(&self) -> u8;
    /// Open the destination for a header that was already verified. `target` is LOADER, STAGE2 or
    /// MODULE (then `name` is the module name); refuse with a [`Reject`] if this device does not take
    /// it, the name is unknown, or `image_size` does not fit. With `dry` nothing will be written.
    fn open(&mut self, target: u8, name: &[u8], image_size: u32, dry: bool) -> Result<Self::Sink, Reject>;
}

/// What the caller does after [`Device::serve`] returned and the connection is closed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Outcome {
    /// Nothing changed (an error was reported, or a dry run).
    None,
    /// An image was written and committed.
    Installed { target: u8 },
    /// A zero-length Main manifest was accepted: set the RTC handoff to [`crate::rtc::HANDOFF_LOADER`]
    /// and restart (the reply was already sent; let it drain first).
    RequestLoader,
    /// A plain reboot request: restart.
    Reboot,
}

pub struct Device<P: Platform, F: FlashOps> {
    pub platform: P,
    pub state: StateLog<F>,
}

fn read_exact<S: Read>(s: &mut S, buf: &mut [u8]) -> Result<(), Reject> {
    s.read_exact(buf).map_err(|_| wire::SHORT_READ)
}

/// The client closed its write side and sent nothing more.
fn expect_fin<S: Read>(s: &mut S) -> Result<(), Reject> {
    let mut b = [0u8; 1];
    match s.read(&mut b) {
        Ok(0) => Ok(()),
        _ => Err(wire::EXPECTED_FIN),
    }
}

impl<P: Platform, F: FlashOps> Device<P, F> {
    pub fn new(platform: P, flash: F) -> Self {
        Device { platform, state: StateLog::open(flash) }
    }

    /// Serve one connection. Writes the reply lines (`P`, `OK`, `ERR`) to the stream.
    pub fn serve<S: Read + Write>(&mut self, s: &mut S) -> Outcome {
        match self.upload(s) {
            Ok(outcome) => outcome,
            Err(r) => {
                let _ = writeln!(s, "ERR {} {}", r.code, r.reason);
                Outcome::None
            }
        }
    }

    fn min_version(&self, target: u8) -> Option<u32> {
        match target {
            target::MAIN => self.state.view.minver,
            target::LOADER => self.state.view.minver_loader,
            target::STAGE2 => self.state.view.minver_stage2,
            _ => None, // modules have no rollback check
        }
    }

    fn upload<S: Read + Write>(&mut self, s: &mut S) -> Result<Outcome, Reject> {
        let started = Instant::now();
        let mut t = [0u8; wire::TRANSPORT_SIZE];
        read_exact(s, &mut t)?;
        let transport = Transport::parse(&t)?;
        if transport.reboot {
            expect_fin(s)?;
            s.write_all(b"OK 0\n").map_err(|_| wire::SHORT_READ)?;
            return Ok(Outcome::Reboot);
        }
        let mut pubkey = [0u8; PUBKEY_SIZE];
        let mut sig = [0u8; SIG_SIZE];
        let mut raw = [0u8; HEADER_SIZE];
        read_exact(s, &mut pubkey)?;
        read_exact(s, &mut sig)?;
        read_exact(s, &mut raw)?;
        let h = Header::parse(&raw);

        // Everything below happens before any flash is touched.
        if !h.magic_ok() || !wire::verify_signature(&pubkey, &sig, &raw) {
            return Err(wire::BAD_SIGNATURE);
        }
        if self.state.view.pin.is_some_and(|pin| pin != pubkey) {
            return Err(wire::KEY_MISMATCH);
        }
        if self.min_version(h.target).is_some_and(|min| h.version < min) {
            return Err(wire::ROLLBACK);
        }
        if h.format != wire::FORMAT || h.cpu != self.platform.cpu() {
            return Err(wire::WRONG_TARGET);
        }

        // A signed Main header with no image: reboot into the loader for an upgrade.
        if h.target == target::MAIN && h.image_size == 0 {
            if h.block_count != 0 {
                return Err(wire::BAD_LENGTH);
            }
            expect_fin(s)?;
            let _ = writeln!(s, "OK 0 request-loader{}", if transport.dry { " dry" } else { "" });
            return Ok(if transport.dry { Outcome::None } else { Outcome::RequestLoader });
        }

        let size = h.image_size as usize;
        let min_size = if h.target == target::MODULE { 1 } else { BLOCK };
        if !matches!(h.target, target::LOADER | target::STAGE2 | target::MODULE) {
            return Err(wire::WRONG_TARGET);
        }
        if size < min_size
            || h.block_size as usize != BLOCK
            || h.block_count as usize != size.div_ceil(BLOCK)
        {
            return Err(wire::BAD_LENGTH);
        }
        let mut sink = self.platform.open(h.target, h.name(), h.image_size, transport.dry)?;

        let table_len = h.block_count as usize * TABLE_ENTRY;
        let mut table = Vec::new();
        table.try_reserve_exact(table_len).map_err(|_| wire::NO_MEMORY)?;
        table.resize(table_len, 0);
        read_exact(s, &mut table)?;
        if Sha256::digest(&table)[..] != h.table_sha256 {
            return Err(wire::BAD_TABLE);
        }

        // Block 0 is held back until everything else has been checked and written.
        let mut head = vec![0u8; size.min(BLOCK)];
        read_exact(s, &mut head)?;
        if Sha256::digest(&head)[..TABLE_ENTRY] != table[..TABLE_ENTRY] {
            return Err(wire::BAD_BLOCK);
        }
        if h.target != target::MODULE && (head.len() < 14 || head[0] != 0xe9 || u16::from_le_bytes([head[12], head[13]]) as u8 != h.cpu)
        {
            return Err(wire::BAD_IMAGE_HEADER);
        }
        let mut whole = Sha256::new();
        whole.update(&head);
        if !transport.dry {
            sink.begin(h.image_size).map_err(|_| wire::FLASH)?;
        }
        let mut body = [0u8; BLOCK];
        let mut offset = head.len();
        let mut index = 1u32;
        while offset < size {
            let n = (size - offset).min(BLOCK);
            read_exact(s, &mut body[..n])?;
            let want = &table[index as usize * TABLE_ENTRY..(index as usize + 1) * TABLE_ENTRY];
            if Sha256::digest(&body[..n])[..TABLE_ENTRY] != *want {
                return Err(wire::BAD_BLOCK);
            }
            whole.update(&body[..n]);
            if !transport.dry {
                sink.write_block(index, &body[..n]).map_err(|_| wire::FLASH)?;
            }
            offset += n;
            index += 1;
            if offset % (64 * 1024) == 0 || offset == size {
                let _ = writeln!(s, "P {} {}", offset, if transport.dry { 0 } else { offset - head.len() });
            }
        }
        if whole.finalize()[..] != h.image_sha256 {
            return Err(wire::BAD_IMAGE_HASH);
        }
        expect_fin(s)?;

        if !transport.dry {
            // Commit: the head sector last, then the state records.
            sink.write_block(0, &head).map_err(|_| wire::FLASH)?;
            sink.finish().map_err(|_| wire::FLASH)?;
            if self.state.view.pin.is_none() {
                self.state.append(REC_PIN, &pubkey);
            }
            match h.target {
                target::LOADER => self.state.append_u32(REC_MINVER_LOADER, h.version),
                target::STAGE2 => self.state.append_u32(REC_MINVER_STAGE2, h.version),
                _ => true,
            };
        }
        let _ = writeln!(s, "OK {} ms={}{}", size, started.elapsed().as_millis(), if transport.dry { " dry" } else { "" });
        Ok(if transport.dry { Outcome::None } else { Outcome::Installed { target: h.target } })
    }
}

// REC_MINVER (Main) is written by the loader that installs Main, not by a running Main.
#[allow(dead_code)]
const _: u8 = REC_MINVER;

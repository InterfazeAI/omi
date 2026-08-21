# DevKit v2 + SD Card — Bring-Up Notes

Hard-won findings from bringing up offline SD recording on a hand-wired DevKit v2. Read this
before debugging recording, sync throughput, or codec behaviour on this board: most of what
follows cost hours to find and is not obvious from the code.

## Hardware Under Test

- Seeed XIAO nRF52840 Sense, Zephyr target `xiao_ble/nrf52840/sense`.
- Adafruit MicroSD Card BFF, hand-wired over SPI (not the board the overlay was written for).
- No speaker fitted; a latching switch in the battery line instead of the momentary button.
  Build with `sd-on-no-button-speaker.conf` so the firmware does not wait on absent hardware.
- SPI is pinned to 8 MHz in `overlay/xiao_ble_sense_devkitv2-adafruit.overlay`. Dropping from
  24 MHz did **not** fix the `-EIO` failures it was meant to (see Trap 1) — it is kept only as
  margin for jumper wires, and is still far faster than the BLE link can drain.

## Build and Flash

```bash
# From the repo root. Produces v2.7.0/build_quiet/zephyr/zephyr.uf2
docker run --rm -v "$(pwd)/omi:/omi" -e CMAKE_PREFIX_PATH=/opt/toolchains \
  ghcr.io/zephyrproject-rtos/ci:v0.26.18 bash /omi/firmware/omi_build.sh
```

`omi_build_diag.sh` is the same image plus `diag-threads.conf` (per-thread CPU accounting).

Flashing is UF2: double-tap RST, wait for `/Volumes/XIAO-SENSE`, then copy the image. Two
macOS quirks waste time here:

- Spotlight indexing the bootloader volume causes `cp: Permission denied`. Run
  `mdutil -i off /Volumes/XIAO-SENSE` first.
- The bootloader ejects the volume the moment it has the whole image, so the write **aborts
  with an I/O error on success**. Treat "volume disappeared" as the success signal, not the
  exit code.

## Traps

Each of these produced a confusing symptom that pointed somewhere other than the cause.

### 1. `fs_sync()` always fails on SD — the error is meaningless

`card_ioctl()` in `v2.7.0/zephyr/subsys/sd/sd_ops.c` has no `break` after its
`DISK_IOCTL_CTRL_SYNC` case, so it falls into `default:` and overwrites the successful result
with `-ENOTSUP`. FatFs reports that as `FR_DISK_ERR`, which surfaces as `-EIO`. **Every**
`fs_sync()` on an SD card fails on this SDK version, on any card, freshly formatted or not.

The flush itself has already happened by then: `f_sync` writes the window, FAT and directory
entry before that final ioctl, and `sdmmc_wait_ready()` does run. So the code carries no
information. Treating it as fatal — closing the file on failure — is what corrupted the card
twice and stopped recording. `storage_sync_locked()` in `sdcard.c` now swallows it. Real write
failures still surface through `fs_write`.

The SDK tree is gitignored, so this is worked around in our code rather than patched. It is a
genuine upstream bug and worth reporting.

*Do not re-diagnose this by reformatting the card. It is not the card.*

### 2. Small writes are pathologically slow — batch them

A 440-byte write lands inside a single 512-byte sector, so the card erases and reprograms a
whole internal page for each one. Measured **~220 ms per 440 bytes**, which is within the
250 ms the SD spec permits for a single block write. At ~4 writes/s that left the filesystem
~89% busy and held `sd_mutex` almost continuously.

`sdcard.c` now accumulates into an 8 KB sector-aligned `write_batch` and flushes once: **~280 ms
per 8 KB**, roughly 12x better per byte, and ~7% filesystem duty. The cost is that a power cut
loses up to 8 KB (~3.5 s) of audio.

### 3. Do not flush the batch on every read refill

The read cache refill must flush only when the read actually reaches into audio still in RAM:

```c
if (offset + READ_CACHE_SIZE > audio_file_size - write_batch_len) {
    write_batch_flush_locked();
}
```

Flushing unconditionally writes a near-empty batch on every refill, which reintroduces exactly
the small random writes that Trap 2 exists to avoid. This dropped sync throughput to 0.1 KB/s.

### 4. Opus SILK/VOIP is slower than real time on this part

Switching to `OPUS_APPLICATION_VOIP` + `CONFIG_OPUS_MODE_HYBRID` for DTX and smaller files
cost **12 ms to encode each 10 ms frame** (`CODEC_PACKAGE_SAMPLES 160` at 16 kHz). The encoder
could not keep up: 82 frames/s produced against 100 needed, consuming 99% of the CPU.

CELT costs ~577 ms/s for the full 100 frames/s. Critically, **CELT at 20 kbps produces the same
file size as SILK+DTX at 24 kbps** (2.2 vs 2.1 KB/s), so the SILK experiment bought nothing and
cost everything. Keep `CODEC_OPUS_APPLICATION OPUS_APPLICATION_RESTRICTED_LOWDELAY` and
`CONFIG_OPUS_MODE CONFIG_OPUS_MODE_CELT`.

### 5. Thread priorities: codec starves everything below it

| Thread | Priority | Blocking behaviour |
|---|---|---|
| codec (`codec.c`) | `K_PRIO_PREEMPT(4)` | sleeps 10 ms only when short of samples |
| pusher (`transport.c`) | `K_PRIO_PREEMPT(7)` | sleeps 2 ms **only when the queue is empty** |
| storage sync (`storage.c`) | `K_PRIO_PREEMPT(7)` | `k_yield()` per packet |

This forms a starvation chain. If the codec saturates the CPU, the pusher never drains its
queue, so it never reaches its sleep, so it never yields to the equally-ranked sync thread.
The sync thread then only runs when the pusher blocks on `sd_mutex` — once per flush. Measured
gaps of **3.7 s between sync iterations**, which is what 0.1 KB/s transfers look like.

Diagnosing this needs per-thread numbers, not inference. `push: iters == got` every second is
the tell that the pusher never sleeps.

### 6. Silent failures that look like dead hardware

- **Not advertising at all, CPU idle.** `main()` returns early when `mount_sd_card()` fails, so
  BLE never starts. Check the card before suspecting the radio.
- **Silent card wipe on boot.** `CONFIG_FS_FATFS_MOUNT_MKFS=y` without `FS_MOUNT_FLAG_NO_FORMAT`
  reformats an inconsistent card on mount. `mount_sd_card()` now passes the flag and creates
  `a01.txt` only if genuinely absent.
- **Watchdog resets / disconnects on notify enable.** `CONFIG_LOG_MODE_IMMEDIATE` plus log spam
  on a USB CDC port nobody is reading blocks the logging thread and starves the watchdog. Use
  deferred logging; `usb-console-quiet.conf` compiles most `LOG_*` out while keeping `printk`.
- **`ASSERT_TRUE(a == b)` never fired.** The macro did not parenthesize its argument, so it
  expanded to `if (!a == b)`. Fixed in `utils.h`; it had been hiding a real encoder overflow.
- **Boot loop after a codec change.** `OPUS_ENCODER_SIZE` in `codec.c` is a hardcoded constant
  per mode; a stale value overflows `m_opus_encoder`. `codec_start()` now prints the size Opus
  actually wants. Also, `codec.c` must include `lib/opus-1.2.1/config.h`, or every
  `CONFIG_OPUS_MODE == ...` test compares 0 to 0 and matches all branches at once.

## Measured Baselines

Use these to tell "slow" from "broken" without re-deriving them.

| Metric | Value |
|---|---|
| Recording rate, CELT 20 kbps | 2,391 B/s (143 KB/min) |
| Recording rate, CELT 32 kbps | 3,997 B/s (240 KB/min) |
| BLE sync throughput | 14.8 KB/s |
| Pull 60 s of audio (20 kbps) | ~10 s |
| Opus encode CPU, CELT | 577–609 ms/s |
| Opus encode CPU, SILK (unusable) | 991 ms/s |
| SD write, 8 KB batch | ~280 ms |
| SD write, 440 B unbatched | ~220 ms |

Anything near 0.1 KB/s on sync means thread starvation (Trap 4/5), not the card.

## On-Card Format

`/SD:/audio/a01.txt` is a raw stream of 440-byte blocks. Within a block, frames are packed as
`[length][payload]`, and every payload begins with the Opus TOC byte — `0xB0` for the current
CELT config, constant because the encoder settings are fixed.

Two things break naive decoders:

- A block boundary carries a **stray length byte** with no payload, since a frame that does not
  fit is restarted in the next block.
- The 440-byte grid can be **shifted** relative to the file start, because any short or dropped
  write in the file's history offsets everything after it.

So a host decoder must resynchronize on the TOC byte rather than trust block alignment:

```python
while i + 1 < len(raw):
    ln = raw[i]
    if raw[i + 1] == 0xB0 and 2 <= ln <= 60 and i + 1 + ln <= len(raw):
        frames.append(raw[i + 1:i + 1 + ln]); i += 1 + ln
    else:
        i += 1  # resync
```

Sidecars: `/SD:/audio/a01.idx` holds 16-byte records (audio offset, epoch, uptime, boot id)
written every 30 s; `/SD:/info.txt` holds the read offset; `/SD:/boot.bin` holds the boot id.

## BLE Protocol (host testing)

| Purpose | UUID |
|---|---|
| Audio service (advertised) | `19b10000-e8f2-537e-4f6c-d104768a1214` |
| Storage service | `30295780-4301-eabd-2904-2849adfeae43` |
| Storage command (write + notify) | `30295781-4301-eabd-2904-2849adfeae43` |
| Storage size (read) | `30295782-4301-eabd-2904-2849adfeae43` |
| Time sync | `19b10030-e8f2-537e-4f6c-d104768a1214` |

The size characteristic returns `<II` — recording length and saved offset. Commands are six
bytes, `[cmd, file_no, off>>24, off>>16, off>>8, off]`, with `READ=0, DELETE=1, NUKE=2, STOP=3`.
A single byte `100` notified back marks end of transfer.

Downloads should start on a 440-byte boundary. `DELETE` used to break recording until reboot;
`clear_audio_file()` now reopens the handle correctly, but prefer reading from an offset over
deleting during testing.

## Host Tools

`../scripts/devkit/sd_sync/` implements all of the above — discovery, download, the resyncing
parser and decode to WAV. Run them from that directory, since they import `omi_sd` as a sibling.

```bash
python3 info.py                                     # size on card, estimated sync time
python3 record_and_pull.py 60 ~/Desktop/take1.wav   # record a window, pull it back
python3 pull_range.py <start> <length> [out.wav]    # re-decode a span already on the card
python3 throughput.py 25                            # sync speed; ~14-16 KB/s is healthy
```

**The older scripts one level up do not work against this firmware, and fail quietly.** They
were written for a previous storage protocol that notified 83-byte packets carrying a 4-byte
header, with frame length at offset 3. This firmware sends the 440-byte blocks described above,
whose framing is `[length][payload]` with no header. Concretely:

- `sdcard_test_files/get_audio_file.py` only writes packets matching `len(data) == 83`, so it
  discards every notification and leaves `my_file.txt` empty — with no error printed.
- `get_audio_file.py` does save the raw blocks, but its frame extraction reads the old header
  offsets, so `audio_frames` is nonsense.
- Both copies of `decode_audio.py` walk the file in a fixed 83-byte stride and take length from
  offset 3. Against 440-byte blocks that yields garbage rather than a clean failure.
- Both hardcode `device_id` to a CoreBluetooth UUID specific to one Mac and one device, and
  always read from offset 0.

`MAX_WRITE_SIZE 440` dates to the first commit of `transport.c`, so these scripts have been
stale for a long time. `docs/doc/developer/DevKit2Testing.mdx` still instructs readers to run
them.

## Still Open

- Each 440-byte block ends in ~12.6 bytes of stale data (measured: one contiguous garbage run
  per block, 3.0% of the stream, and consecutive runs are the same bytes shifted by one or two —
  leftovers from the previous block, since `write_to_storage()` only overwrites the front of
  `storage_temp_data`). **Not worth changing.** Both app consumers already stop at the boundary
  (`storage_sync.dart` and `ring_protocol.dart` break on `offset + 1 + size >= 440`, which
  exactly matches the firmware's overflow rule), so nothing mis-parses it. Zero-filling the tail
  would not reclaim the space either; that would need frames to span blocks, which breaks the
  "each block parses independently" contract both paths rely on.
- The Zephyr `card_ioctl` fall-through is worked around, not fixed upstream.
- **A genuinely failing card is currently indistinguishable from that SDK bug.** Because
  `storage_sync_locked()` swallows every `fs_sync` error, `sync_error_count` (`sdcard.c`) is the
  only signal that could tell a real fault from the known false one — and nothing reads it.
  Deliberately accepted; log it periodically if card faults are ever suspected.
- Dead code in `transport.c`: the file-scope `static uint32_t offset` is never read or written,
  and the init `memset(storage_temp_data, 0, OPUS_PADDED_LENGTH * 4)` clears 320 bytes of a
  440-byte buffer. Harmless (statics are zero-initialized) but it reads like tail-clearing that
  does not happen — see the first bullet before concluding the tail is meant to be cleared.
- Mic gain is `MIC_GAIN 64` (+12 dB; register step is 0.5 dB from 40 = 0 dB). Test recordings
  clip at 0.0 dBFS on close speech, which is permanent distortion. Level does not meaningfully
  affect file size — a 14 dB louder take changed the data rate by 0.6% — so lowering it to ~52
  (+6 dB) would cost no capacity. Kept at 64 by choice.
- 32 kbps was judged clearly better on soft sounds than the 20 kbps currently configured.

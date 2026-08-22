# DevKit v2 + SD Card — Bring-Up Notes

Hard-won findings from bringing up offline SD recording on a hand-wired DevKit v2. Read this
before debugging recording, sync throughput, or codec behaviour on this board: most of what
follows cost hours to find and is not obvious from the code.

## Hardware Under Test

- Seeed XIAO nRF52840 Sense, Zephyr target `xiao_ble/nrf52840/sense`.
- Adafruit MicroSD Card BFF, hand-wired over SPI (not the board the overlay was written for).
  Chip select runs from the BFF's square CS pad to **D0/A0 (P0.02)**, matching the base overlay.
  That wire is the single most failure-prone thing on the board and its failure mode looks exactly
  like a dying SD card — read Trap 10 before debugging any write failure. A future build can
  delete the wire entirely: bridging the BFF's **A0** pin-select solder jumper (front of the board,
  below the card slot, alongside RX and A1) routes chip select to the BFF's A0 edge pad, which
  already sits on the XIAO's A0/D0 pad in the stack. Same P0.02, so no firmware change — but that
  stack joint stops being merely structural and must be reflowed as a real electrical joint.
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
`omi_build_secure.sh` is the same image plus `secure-pairing.conf`, which requires a paired,
encrypted link for audio, recordings, the clock and DFU. `omi_build_unbond.sh` erases every bond at
boot; it is the recovery path when the bonded device is gone and cannot release the slot itself.

Use the image tag above verbatim. `zephyr-build:v0.26.13` is **not** on this machine and asking for
it silently starts a multi-gigabyte pull that looks like an extremely slow build.

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

**It is not only `fs_sync`.** Every FatFs call that ends in a flush hits the same ioctl, so
`fs_unlink` and `fs_rename` also return `-EIO` *after doing the work*. This cost a full session:
ring eviction reported failure on every rotation, the loop bailed out to avoid leaking a file it
believed was still there, and the segment count grew without bound — while each unlink had in
fact deleted the file. The giveaway was that the oldest sequence number advanced by exactly one
per reboot.

Never branch on the return code of these calls. `path_absent_locked()` in `sdcard.c` asks the
filesystem what actually happened instead:

```c
int rc = fs_unlink(path);
if (rc && !path_absent_locked(path)) { /* only now is it a real failure */ }
```

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

### 6. Mount runs with nothing feeding the watchdog

`main()` calls `watchdog_init()` (30 s) before `mount_sd_card()`, but `watchdog_feed()` only runs
in the main loop, which is reached after every init step. So all of mount is one unfed window.
Reclaiming a large backlog of over-budget segments there exceeded it: the device reset mid-mount,
trimmed a few more on the next boot, and reset again — presenting as a boot loop with
`Reset by WATCHDOG` and no BLE, because a failed or incomplete mount stops advertising entirely.

`SEG_BOOT_TRIM_MAX` bounds how much mount will reclaim; the rest drains through normal rotation,
which runs on the storage thread where a slow eviction cannot starve the watchdog. **Anything new
added to mount must be bounded the same way.**

### 7. Never touch the filesystem from a GATT handler

`CONFIG_BT_RX_STACK_SIZE` is 1024 (the config carries two commented-out attempts to raise it to
4096, so this has bitten before). GATT write and read handlers run on that stack, and a FatFs
directory walk does not fit — the device hard-faults and reboots, which over BLE looks only like
`bleak.exc.BleakError: disconnected`.

This hid for a long time behind an asymmetry: `get_file_size()` answers for the segment being
recorded from a RAM counter and never touches the card, so reading the newest segment — the only
thing the tools did — was safe. **Every other target calls `fs_stat`**: any older segment, and
any timestamp index. Segment rotation turned that latent bug into a routine one, since a normal
sync walks older segments.

Size lookups now happen in `setup_storage_tx()` on the storage thread, which also sends
`ZERO_FILE_SIZE`/`END_OF_TRANSFER` directly, because the transfer loop only speaks when it has
bytes. Keep it that way: the write handler must stay a cheap parse-and-latch.

**A second instance shipped at the same time**, found only by auditing for the first: setting the
clock (`time_sync_write_handler` → `rtc_set_epoch` → `storage_index_mark`) wrote an index record —
`fs_open`/`fs_write`/`fs_sync`/`fs_close`, more filesystem work than the `fs_stat` above — on the
same 1 KB stack. It would have faulted the device the first time the app synced time, which is the
one thing that makes the index worth having. `storage_index_mark()` now only latches a request and
`storage_index_service()` performs it on the storage thread, so the API is safe from any caller
rather than just that one.

A third is now caught automatically. `.github/scripts/check_ble_handler_filesystem.py` builds a
call graph from every registered GATT and connection callback and fails if any route reaches
`fs_*`/`disk_access_*`; it runs locally and in CI through `.github/checks-manifest.yaml`. It was
validated by reintroducing both bugs above and confirming each is reported with its call chain —
its own first draft passed while bug one was present, because devkit and omi share 104 function
names and one image's stub masked the other's implementation, so the images are analysed
separately.

Reboots are easy to miss from the host — watch the counters on the info characteristic
(`sync errors` resetting to near zero is the tell) rather than assuming a disconnect was the radio.

### 8. Serial logging is not a reliable observation channel here

Two independent reasons, both of which wasted time in this session:

- The base config sets `CONFIG_CONSOLE=n`, so a fragment that only raises
  `CONFIG_LOG_DEFAULT_LEVEL` produces **no output at all**. Layer `debug-usb-log.conf` in too.
- Even with the console on, the log thread runs below the codec (Trap 5), so once recording
  starts it may never be scheduled. In practice you get the boot log up to exactly
  `CONFIG_USB_CDC_ACM_RINGBUF_SIZE` (1024) bytes and then silence.

For anything that happens during steady-state recording, **publish a counter on the storage-info
characteristic and read it over BLE** instead. That is how the eviction bug in Trap 1 was finally
pinned down, and why `evictions`, `last eviction errno` and `sync errors` are in that payload.

### 9. Silent failures that look like dead hardware

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

### 10. An intermittent chip-select wire impersonates a dying card

The symptom, in order: the card initializes, the filesystem mounts, and the directory scan finds
the segments and reports their sizes — all real block reads, all successful. Recording runs
normally for a while. Then one or two `fs_write` calls fail with `-5`, and from that moment
**every** `fs_open` fails with `-5` too, indefinitely, while the byte count sits frozen. The
open failures keep trickling in at roughly four per minute forever.

The cause was a hand-soldered chip-select wire that was mechanically failing. It was only ever
identified because it finally broke outright.

Reads survive and writes do not because of duration. A read or a command holds CS asserted for a
few hundred microseconds; a block program holds it through the data token, the payload, and a
busy-wait the SD spec permits to reach 250 ms. An intermittent joint passes the short transactions
and glitches the long one, and a card interrupted mid-program hangs until it is power-cycled —
which is exactly the endless-open-failure tail. The joint beeps fine on a continuity test the
whole time, because it is only open under flex or thermal movement.

**The false trail matters more than the fault.** Because the failure was intermittent, it
manufactured convincing evidence for conclusions that were wrong:

- Moving CS from P0.02 to P1.11 appeared to *cause* the failure, and moving it back appeared to
  *fix* it — complete with a rebuild, a reflash, and a passing 60 s growth test. The pin was
  innocent. The same failing wire was in both paths, and the "fix" held for three and a half
  minutes before the identical failure returned.
- **A 60 s test proves nothing here.** On the run that looked fixed, the failure took ~3.5 minutes
  to appear. Run at least six minutes with no downloads before believing any card fix.

None of the following changed the symptom, and none are worth repeating:

- Two different microSD cards, each freshly formatted FAT32/MBR and each verified good on a Mac
  with a 200 MB write-read checksum.
- SPI at 1 MHz instead of 8 MHz, ruling out timing margin.
- `WRITE_BATCH_SIZE` cut from 8192 to 512 so every write is a single sector, ruling out
  multi-block CMD25.
- Three reformats, the card reseated, the battery and switch removed to run on USB alone, and the
  3V/GND joints reflowed. The BFF's 3V pad measured a solid 3.3 V throughout.
- Two chip-select routings, with a separate firmware build for each.

So when writes fail while reads work, **prove the CS wire mechanically before changing anything in
software.** Wiggle it while recording and watch the byte counter stall, or simply reflow the joint
and add strain relief. Then confirm with a six-minute no-download run.

Two BFF wiring facts, because each generated its own dead end:

- On the BFF the **TX pad and the square CS pad are the same net** — a factory jumper ties them.
  Swapping between the two is not a variable; both are places to grab the same signal.
- The **A0 pad is not chip select** unless its solder jumper is bridged. Wiring XIAO D0 to the
  BFF's A0 pad leaves CS floating, which presents differently: `CMD0` times out, the mount fails,
  and because a failed mount stops BLE (Trap 9) the board looks dead in a watchdog reboot loop.

This was only diagnosable because the open and write failure counts and their errnos are published
on the storage-info characteristic — Trap 8 again. Serial gave nothing. Without counters that
separate opens from writes, the "writes fail first, then opens fail forever" ordering is invisible,
and that ordering is the whole signature.

Healthy baseline after the joint was repaired, for comparison: six minutes of recording at
2.77 KB/s with zero I/O failures, then a 366 KB pull at 13.7 KB/s producing 13,191 frames with no
decode errors, with the counters still at zero afterwards and recording uninterrupted throughout.

### 11. `BT_GATT_PERM_*_ENCRYPT` without `CONFIG_BT_SMP` bricks the device silently

The board advertises, connects, and answers the unencrypted battery characteristic, but every
useful characteristic returns ATT error 15, `Insufficient Encryption`. Reflashing does not fix it.
Rebooting does not fix it. Toggling the host's Bluetooth does not fix it.

The cause is a build where the GATT attributes carry the `_ENCRYPT` permission variants while SMP
is compiled out. Zephyr honours the permission regardless; with no Security Manager there is no way
to ever establish encryption, so the attribute is unreachable for the life of the image. The device
is behaving exactly as told.

It is a nasty one because the symptom impersonates a host problem. `Insufficient Encryption`
arriving from a device that cannot do encryption reads as a stale GATT cache on the central, and
macOS genuinely does cache service databases, so the false explanation is plausible enough to
survive a Bluetooth restart and a reflash before anyone doubts it. Two things break the illusion:
the error persists after flashing an image built with SMP off, and it appears on the very first
connection from a central that has never seen the device.

The fix is that permissions must track the Kconfig, never be hardcoded. `transport.h` defines
`OMI_PERM_READ` / `OMI_PERM_WRITE`, which expand to the `_ENCRYPT` variants only under
`CONFIG_BT_SMP`. Use those in every `BT_GATT_CHARACTERISTIC` and `BT_GATT_CCC`; a literal
`BT_GATT_PERM_READ_ENCRYPT` in `storage.c` or `transport.c` is a bug. This matters specifically
because pairing lives in an optional fragment (`secure-pairing.conf`), so the SMP-off build is a
configuration that ships, not a hypothetical.

### 12. Pairing does not fail, it starves — the codec outranks the ECDH thread

Pairing from a host produces a prompt, the prompt is accepted, and then nothing happens. The
encrypted read eventually times out on the host and the device reports `BT_SECURITY_ERR_UNSPECIFIED`
(reason 9) with the link still at security level 1 and the bond slot still empty.

"Unspecified" is a red herring. `smp.c` has roughly forty sites that return it, but only one is
reachable here: `smp_timeout()` calls `smp_pairing_complete(smp, BT_SMP_ERR_UNSPECIFIED)`. The
pairing was never refused — it stalled and hit the 30 s SMP limit. Time the failed attempt: about
thirty seconds of dead wait is the confirmation, and it distinguishes this from a real rejection,
which returns immediately.

The stall is a priority inversion between the application and the Bluetooth controller:

| Thread | Priority |
|---|---|
| codec (`codec.c`) | `K_PRIO_PREEMPT(4)` |
| BT CTLR ECDH (`nrf/subsys/bluetooth/controller/ecdh.c`) | `K_PRIO_PREEMPT(10)` |

LE Secure Connections needs a P-256 ECDH computation before pairing can complete, and nrfxlib runs
it on that dedicated low-priority thread. This device records continuously from the moment it powers
on, so the codec is always runnable and always wins. Trap 5 measured multi-second scheduling gaps at
priority 7; priority 10 does worse, and the computation never finishes inside the pairing window.
Because recording never stops, pairing fails **every** time rather than intermittently, which makes
it look like a protocol or host bug instead of a scheduling one.

Fix: `CONFIG_BT_CTLR_ECDH_IN_MPSL_WORK=y` in `secure-pairing.conf`, which runs the computation in
the MPSL workqueue — above the codec instead of below it. The documented cost is delaying MPSL work,
which is acceptable for a one-off computation that only happens at pairing time.

Four plausible causes were wrong, and each cost a cycle. Do not re-litigate them:

- **The host.** macOS was blamed twice. It was never involved.
- **The bond slot.** `BT_MAX_PAIRED=1` with no eviction genuinely does refuse new pairings when
  full, so "a stale bond from an earlier attempt" is a compelling story. The diagnostics said 0 of 1
  slots used, killing it in one read.
- **`CONFIG_BT_SMP_ENFORCE_MITM`.** It looks fatal for a device with no display or keypad. It is
  not: `smp.c` explicitly clears the MITM bit when IO capability is `NoInputNoOutput`, so the
  setting is a no-op on this board.
- **A missing crypto backend.** `CONFIG_BT_ECC` and `CONFIG_BT_CTLR_ECDH` (Oberon) are both present.

The reason this was findable at all is the pairing-status characteristic, which is deliberately
readable on an **unencrypted** link. A diagnostic that requires the thing it diagnoses is useless,
and serial was mute throughout — the SMP debug image does not even boot (it hangs after the reset
reason, before `watchdog_init()`, so no watchdog reset ever fires to reveal it). Trap 8 again: on
this board, BLE is the only observation channel that works.

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

Recording is a **ring of segment files**, `/SD:/audio/aNNNNN.txt`, so it never has to stop: when
the ring is full the oldest segment is unlinked and a new one is started. Each segment is a raw
stream of 440-byte blocks, exactly as the single file used to be — nothing about block or frame
parsing changed.

Why segments rather than wrapping inside one fixed-size file:

- Every write stays a sequential append, so the 8 KB batching in Trap 2 still applies unchanged.
  Wrapping would mean seek-back writes whose cost on this card was never measured.
- The write position after a reset is just "the highest-numbered file", so nothing has to be
  persisted per batch. Persisting a write cursor would have reintroduced exactly the small
  random writes that Trap 2 exists to avoid.
- Evicting is one `fs_unlink`, not a rewrite.

The cost is that eviction is coarse — a whole segment disappears at once. That is why the
default is 128 segments rather than a handful: you lose under 1% of retained audio at a time.

Geometry is derived at mount from `fs_statvfs`, so one image suits any card: 90% of free space,
divided into `CONFIG_OMI_RING_SEGMENT_COUNT` segments, each rounded to a multiple of **450,560
bytes** — `lcm(8192, 440)`, so a batch flush never straddles a segment and the 440-byte grid
never shifts inside one. Segment size is clamped to 3.4 MB–220 MB. Existing segments keep
whatever size they were written with; only new ones follow the current target.

Set `CONFIG_OMI_RING_SEGMENT_BYTES` to force a size. `ring-fast-rotate.conf` uses the 450,560
minimum with 3 segments, which turns the whole ring over in about 9 minutes — build it with
`omi_build_ringtest.sh`. Rotation and eviction are otherwise a once-a-day event and effectively
untestable.

Two things follow from the ring that callers must respect:

- **Segment numbers are positions, not identities.** `file_num` is 1-based and ordered oldest
  first, so an eviction or a delete shifts everything above it down. Use the sequence numbers
  from the info characteristic to track a segment across time.
- Offsets are **within a segment**, not across the ring.

A card written by the pre-ring firmware has a single `a01.txt`; it is renamed to `a00001.txt` at
mount rather than orphaned.

Within a block, frames are packed as `[length][payload]`, and every payload begins with the Opus
TOC byte — `0xB0` for the current CELT config, constant because the encoder settings are fixed.

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

Sidecars: each segment has `/SD:/audio/aNNNNN.idx` holding 16-byte records (offset **within that
segment**, epoch, uptime, boot id) written every 30 s and on rotation; it is unlinked with its
segment, so the index cannot outgrow the ring. `/SD:/info.txt` holds the read offset;
`/SD:/boot.bin` holds the boot id.

## BLE Protocol (host testing)

| Purpose | UUID |
|---|---|
| Audio service (advertised) | `19b10000-e8f2-537e-4f6c-d104768a1214` |
| Storage service | `30295780-4301-eabd-2904-2849adfeae43` |
| Storage command (write + notify) | `30295781-4301-eabd-2904-2849adfeae43` |
| Storage size (read) | `30295782-4301-eabd-2904-2849adfeae43` |
| Time sync | `19b10030-e8f2-537e-4f6c-d104768a1214` |

The size characteristic returns 50 bytes describing the ring. The first two words keep their
original meaning so a client that reads only 8 bytes still works:

| Offset | Type | Meaning |
|---|---|---|
| 0 | `u32` | bytes in the segment being recorded |
| 4 | `u32` | read offset the device last saved |
| 8 | `u8` | segments on the card — also the number of the newest |
| 9 | `u32` | sequence number of the oldest segment |
| 13 | `u32` | sequence number of the newest segment |
| 17 | `u32` | current segment size target |
| 21 | `u8` | segment cap — `count` above this means eviction is failing |
| 22 | `u32` | successful evictions since boot |
| 26 | `i32` | errno of the last failed eviction, 0 if none |
| 30 | `u32` | `fs_sync` failures — expected nonzero, see Trap 1 |
| 34 | `u32` | `fs_open` failures — unlike sync, any nonzero value is a real fault |
| 38 | `i32` | errno of the last failed open, 0 if none |
| 42 | `u32` | `fs_write` failures |
| 46 | `i32` | errno of the last failed write, 0 if none |

The open and write counters are the pair that make a genuinely broken card distinguishable from
the SDK bug in Trap 1, and their ordering is diagnostic in itself: writes failing first and opens
failing afterwards is the signature in Trap 10. `info.py` prints them with a hint for the common
errnos (`-5`, `-116`, `-2`, `-28`).

Commands are six bytes, `[cmd, file_no, off>>24, off>>16, off>>8, off]`, with
`READ=0, DELETE=1, NUKE=2, STOP=3`. A single byte `100` notified back marks end of transfer.

`file_no` is a segment position, 1 to the count above. Setting its high bit (`| 0x80`) reads that
segment's timestamp index instead of its audio — the index is no longer a reserved file number,
because every number is now a real segment.

Downloads should start on a 440-byte boundary. `DELETE` used to break recording until reboot;
deleting the segment being recorded now opens a fresh one immediately, but prefer reading from an
offset over deleting during testing.

### Pairing service `19b10040-e8f2-537e-4f6c-d104768a1214`

Only present in a `secure-pairing.conf` build. Two characteristics with deliberately opposite
permissions.

`19b10041` — status, 25 bytes, **read without encryption on purpose**. This is the channel that
made Trap 12 diagnosable; gating it behind pairing would make it useless exactly when it is needed.
It exposes counters and error codes only, never content.

| Offset | Type | Meaning |
|---|---|---|
| 0 | `u8` | layout version, currently 1 |
| 1 | `u8` | flags: `0x01` SMP built in, `0x02` bondable, `0x04` bonds persist, `0x08` this link encrypted |
| 2 | `u8` | bond slots used |
| 3 | `u8` | bond slots total (`CONFIG_BT_MAX_PAIRED`) |
| 4 | `u8` | `bt_security_err` of the last pairing attempt |
| 5 | `u8` | `bt_security_err` of the last security change |
| 6 | `u8` | security level reached by that change |
| 7 | `u8` | security level of the current link |
| 8 | `u32` | connections since boot |
| 12 | `u32` | pairings that succeeded |
| 16 | `u32` | pairings that failed |
| 20 | `u32` | bond releases requested |
| 24 | `i8` | result of the last release |

Reason 9 (`unspecified`) with the level stuck at 1 and an empty bond slot is the Trap 12 signature.

`19b10042` — release, write, **encrypted**. Write exactly the eight bytes `OMIUNBND` to give up the
bond slot so another device can pair. Encrypted because only the current owner may hand the device
over; the magic exists so a truncated or stray write cannot orphan the board.

**This erases every recording.** Releasing the bond hands the device to someone else, and they must
not inherit the previous owner's audio, so the card is wiped as part of the release. The sequence is
three threads on purpose:

1. GATT handler (Bluetooth RX thread) validates the magic and calls `storage_request_unbond_wipe()`.
   It touches nothing else — filesystem or NVS work on this stack is Trap 7.
2. Storage thread runs `clear_audio_directory()`, resets the saved offset, then calls
   `transport_finish_unbond()`.
3. System work queue runs `bt_unpair()` and drops the connection.

The wipe must complete **before** the bond is released. Losing power midway then leaves the old bond
intact, so no new device can pair and read what survived; releasing first would open exactly that
window.

This is the normal way to change owner. `omi_build_unbond.sh` remains the recovery path for when
the bonded device is lost or broken and can no longer ask.

## Host Tools

`../scripts/devkit/sd_sync/` implements all of the above — discovery, download, the resyncing
parser and decode to WAV. Run them from that directory, since they import `omi_sd` as a sibling.

```bash
python3 info.py                                     # segments, retention, estimated sync time
python3 record_and_pull.py 60 ~/Desktop/take1.wav   # record a window, pull it back
python3 pull_range.py <start> <len> [out.wav] [seg] # re-decode a span already on the card
python3 throughput.py 25                            # sync speed; ~14-16 KB/s is healthy
python3 pairing.py                                  # bond slots, pairing errors (works unpaired)
python3 pairing.py --pair                           # trigger the host's pairing prompt
python3 pairing.py --release                        # hand the bond slot to a new device
```

`pairing.py` reads without needing a bond, so it is the first thing to run when pairing misbehaves;
`--release` needs the encrypted link, since only the current owner may give the device up.

`record_and_pull.py` handles a rotation landing inside its window by fetching each part and
joining them, which is the main thing worth exercising with the `ring-fast-rotate.conf` build.

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
- `sync_error_count` on its own still cannot tell a failing card from the SDK bug in Trap 1, since
  that bug makes every sync "fail" — only the rate differs. The confirming signal it needed now
  exists alongside it: the `fs_open` and `fs_write` counters on the same characteristic, which the
  SDK bug never touches.
- Dead code in `transport.c`: the file-scope `static uint32_t offset` is never read or written,
  and the init `memset(storage_temp_data, 0, OPUS_PADDED_LENGTH * 4)` clears 320 bytes of a
  440-byte buffer. Harmless (statics are zero-initialized) but it reads like tail-clearing that
  does not happen — see the first bullet before concluding the tail is meant to be cleared.
- Mic gain is `MIC_GAIN 64` (+12 dB; register step is 0.5 dB from 40 = 0 dB). Test recordings
  clip at 0.0 dBFS on close speech, which is permanent distortion. Level does not meaningfully
  affect file size — a 14 dB louder take changed the data rate by 0.6% — so lowering it to ~52
  (+6 dB) would cost no capacity. Kept at 64 by choice.
- 32 kbps was judged clearly better on soft sounds than the 20 kbps currently configured.
- There is no serial-log image for Security Manager debugging. One was written and deleted: it hung
  after printing the banner and the reset reason, before `watchdog_init()`, so the board sat there
  advertising nothing and not even resetting. It was never needed — Trap 12 was diagnosed entirely
  through the unencrypted pairing-status characteristic, which works on a board you cannot pair
  with and cannot read serial from. Extend that characteristic before reaching for a serial console.
- Link security is opt-in via `secure-pairing.conf` and is not in the default image. Turning it on
  makes every client re-pair, so it should land together with the app-side change. The app has no
  pairing awareness at all yet, including the stale-bond case after a release.
- Kconfig fragments fail silently when their dependencies are missing, and nothing warns. This has
  already shipped a broken image once: `erase-bonds.conf` on its own cannot select
  `CONFIG_OMI_ERASE_BONDS_ON_BOOT`, which depends on `BT_SETTINGS` from `secure-pairing.conf`, so
  the recovery image built cleanly, booted, and erased nothing — a recovery path that quietly does
  not recover. **After changing any build script, grep the generated `.config` for the symbol you
  expected rather than trusting that the build succeeded.**
- Code guarded by `CONFIG_BT_SMP` must be guarded in both directions. `struct bt_conn_cb` has no
  `security_changed` member without SMP, so an unguarded assignment breaks the default image while
  the secure one still builds — build all three images (`omi_build.sh`, `omi_build_secure.sh`,
  `omi_build_unbond.sh`) before believing a Bluetooth change compiles.

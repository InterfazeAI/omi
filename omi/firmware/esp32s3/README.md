# Omi ESP32-S3 Pendant Firmware

A DevKit2-equivalent Omi pendant on the **Seeed XIAO ESP32-S3 Sense**: BLE live
audio, always-on SD journaling with offline sync, WiFi OTA, and battery
reporting. No camera, and no button — power is a hardware slide switch, as on
DevKit1.

The Flutter app treats this device as a plain `DeviceType.omi` and needs no
changes to talk to it, provided the firmware reports exactly the right things
(see [App compatibility contract](#app-compatibility-contract)).

## Why this is a fork of `omiGlass/firmware`, not a port of `omi/firmware/devkit`

`omi/firmware/devkit` is Zephyr/nRF-only at every layer: `nrfx_pdm` in `mic.c`,
direct `NRF_POWER` register writes, ARM assembly in the Opus build, `NRF_PSEL()`
devicetree overlays. Only the BLE protocol and the storage wire format port over.

`omiGlass/firmware` is already `board = seeed_xiao_esp32s3` with a working PDM
mic, Opus encoder, Omi BLE audio service, battery service, and WiFi OTA, so it
supplies the platform layer. The storage protocol comes from a second source: the
consumer CV1 firmware in `omi/firmware/omi/`
([`storage.c`](../omi/src/lib/core/storage.c), [`sd_card.c`](../omi/src/sd_card.c)),
which already implements the ring protocol this build speaks.

## Hardware

| Function | Pin | Notes |
|---|---|---|
| PDM mic clock | GPIO42 | Built into the Sense expansion board |
| PDM mic data | GPIO41 | |
| SD SCK / MISO / MOSI | GPIO7 / GPIO8 / GPIO9 | Requires the J3 solder pads bridged (default from factory) |
| SD chip select | GPIO21 | **Also the XIAO's onboard user LED** — the LED cannot be used |
| Status LED | GPIO1 (A0) | External LED, active low. Freed by removing the button |
| Battery divider | GPIO2 (A1) | `omiGlass/firmware/readme.md` claims A0; it is wrong, since that firmware wired its button to A0/GPIO1 |
| Power | Slide switch in the battery line | Invisible to firmware |

## Build and flash

```bash
pio run -e seeed_xiao_esp32s3                  # build
pio run -e seeed_xiao_esp32s3 -t upload        # flash over USB
pio device monitor --baud 921600               # serial log
python3 scripts/build_uf2.py --env uf2_release # release build + UF2
```

Environments: `seeed_xiao_esp32s3` (default), `seeed_xiao_esp32s3_slow` (115200
baud upload, verbose logs), `uf2_release` (optimised, used by `build_uf2.py`).

The 8 MB flash is split into two ~3.9 MB OTA slots plus a coredump partition
(`partitions_ota.csv`). There is no SPIFFS partition — nothing mounts one, and
audio lives on the SD card.

## Data flow

Every frame is journalled to the card first, and a single `read_seq` cursor
decides which of the two BLE paths delivers it, so no audio is ever sent twice.

```
PDM mic → I2S 16kHz mono → pre-roll ring (500ms) → VAD gate
                                                     │ closed → drop, no encode
                                                     └ open   → Opus (320-sample frames, DTX)
                                                                  → 444-byte record
                                                                     → ring.dat on SD
                                                                        ├ caught up + subscribed → live over 19B10001, self-advance read_seq
                                                                        └ backlog                → app pulls it via ring protocol 30295780
```

### Source layout

| File | Role |
|---|---|
| `app.cpp` | BLE server, record assembly, live drain, the audio and storage tasks |
| `mic.cpp` | I2S PDM capture |
| `opus_encoder.cpp` | PCM ring, 20 ms framing, Opus encode with DTX |
| `vad.cpp` | Energy gate with hysteresis, pre-roll, hangover |
| `sd_ring.cpp` | Fixed-capacity 444-byte record ring on FAT, A/B metadata |
| `storage_service.cpp` | Ring BLE protocol on service 30295780 |
| `rtc_clock.cpp` | Wall clock from the app's time sync, restored estimate on boot |
| `ota.cpp` | WiFi + HTTPS OTA |

## Voice activity gating

Recording is gated on speech. Storage is not the reason — at ~4.4 KB/s a 32 GB
card holds roughly twelve weeks of continuous audio. CPU is: Opus encoding is the
dominant compute cost and skipping it during silence is worth an appreciable
share of the connected current draw.

Two parameters do almost all the work (`config.h`):

- **Pre-roll** (`VAD_PREROLL_MS`, 500 ms). When the gate opens, the buffered
  audio from *before* it opened is encoded first. Without this the leading
  syllable of every utterance is clipped, which degrades transcription quality
  rather than breaking anything, so it fails silently.
- **Hangover** (`VAD_HANGOVER_MS`, 1200 ms). The gate stays open after speech
  stops, so trailing words survive and the natural pauses between words do not
  fragment one sentence into several sessions.

A **session** is one contiguous open period of the gate. Its first record's
timestamp is the true wall-clock start of that speech.

`OPUS_SET_DTX(1)` is deliberately redundant with the gate: closed-gate silence is
never encoded at all, so DTX only earns its keep *inside* a session — exactly
where clipping risk forces the gate to stay open.

## Storage protocol

Service `30295780-4301-EABD-2904-2849ADFEAE43`, `...81` Write+Notify, `...82`
Read. Records are **444 bytes: `[timestamp BE32][440-byte payload]`**, the payload
packed `[len:1][opus frame:len]...` and zero-padded.

Three details are easy to get subtly wrong:

- The 16-byte status read on `...82` is the only **little-endian** part of the
  protocol. It is served from a cached snapshot refreshed on a timer, because a
  GATT read must never block on SD I/O. It must return 16 bytes even when the
  ring is empty, or the app hides the entire Offline Sync UI.
- DATA notifications are **not** record-aligned. They are sliced to `MTU-4` and
  the app's `RingRecordReassembler` puts records back together. The flip side is
  that a single lost or substituted notification misaligns the reassembler for
  the rest of the transfer instead of costing one record — which is why the write
  callback only ever *queues* an ack for the storage task, and never touches the
  control characteristic that the storage task is concurrently driving.
- The payload boundary test is `>=`, not `>`. A frame ending exactly at byte 440
  is silently dropped by the app, so the record builder never places one there
  (`RING_PAYLOAD_FRAME_END_LIMIT`).

### On-disk layout

The CV1 reference writes raw 512-byte sectors with no filesystem, so its
batch-header layer is an artifact of sector granularity that FAT does not need.
Here:

- One fixed-capacity file `/ring.dat`; a record lives at
  `(seq % capacity_packets) * 444`. Capacity is fixed at first init and stored in
  metadata, because changing it would reinterpret every existing offset.
- `read_seq` / `write_seq` / `dropped_packets` live in `/ring.met` with **A/B
  slots, a generation counter, and a CRC32**. Two alternating slots are enough on
  FAT; the CV1's 64-slot rotation is wear-levelling for raw flash.
- On overrun the read cursor is force-advanced past the overwritten records and
  `dropped_packets` grows by the same amount. Recording never stalls waiting for
  the phone.

### Surviving a power cut

This is the first Omi build to combine a **hard power switch with an SD card**:
DevKit1 has a switch but no card, DevKit2 has a card but a firmware-controlled
button that can flush and unmount cleanly. The card can therefore lose power
mid-write at any instant.

The ordering rule is what makes that safe: records are flushed to the card
*before* the metadata that publishes them. Metadata never claims a record the
card has not taken, so after a cut the ring is short by at most the records
written since the last publish (≤ 8 records / ≤ 1 s) and never structurally
corrupt. A card whose filesystem is unreadable is reformatted on mount rather
than taking offline recording down with it.

### Single delivery cursor

The app runs live capture and offline sync concurrently with no arbitration, and
its only dedup is an exact `device_timerStart` WAL-id collision. The backend's
`dedupe_segments_for_merge` only runs *after* synced audio matches an existing
conversation through a ±2-minute window. So duplication has to be prevented here,
structurally:

- **Caught up** (`read_seq == write_seq`) **and audio subscribed** → the record
  just written is unpacked and streamed over `19B10001`, then `read_seq`
  self-advances. Latency is one record, ≤ 400 ms.
- **Small backlog** (≤ `LIVE_CATCHUP_MAX_RECORDS`) → stream the *oldest* unread
  records off the card until the cursor rejoins the writer. They arrive without
  timestamps, which is why the bound is small enough that filing them as "now" is
  honest.
- **Larger backlog** → do not live-drain. Those records stay unread so the app
  pulls them through the ring protocol, which carries per-record timestamps that
  live streaming cannot.
- **Sync conversation in progress** → live drain stands down entirely. This spans
  more than the transfer: the app snapshots `read_seq` with `CMD_RING_INFO` and
  then quotes it back in `CMD_RING_READ`, so a cursor that moves in between makes
  the read's `start_seq` stale and the firmware rejects it as
  `SEQ_OUT_OF_RANGE`. An INFO therefore latches sync intent through DONE and the
  app's closing `CMD_RING_ADVANCE`. The latch also expires on its own, so a peer
  that asks for INFO and goes quiet cannot switch live audio off for good.

The catch-up rule is not an optimisation, it is what makes the cursor converge. A
sync transfers only the range that existed when it started, while the gate keeps
journalling throughout, so **every sync ends with a residual backlog by
construction**. Without catch-up, `read_seq == write_seq` would never hold again
and live audio would stay off for the rest of the connection. The app will not
rescue it either: it skips rings it estimates at under ten seconds of audio, so
`LIVE_CATCHUP_MAX_RECORDS` is **derived from that estimate rather than picked** —
`refreshWalsFromDevice` assumes 160-byte frames and so two frames per record,
putting its floor at 250 packets, not at the ~25 a duration-based reading would
suggest. Any ceiling below 250 leaves a range that neither side drains.

Advancing `read_seq` marks data consumed, it does not erase it — the bytes stay
in `ring.dat` until the write pointer laps them, which is the rolling-archive
retention behaviour we want.

## Clock

There is no RTC. The app writes a 4-byte little-endian UTC epoch to service
`19B10030` / characteristic `19B10031` on every connect; `19B10032` reads it back.

The CV1 reference **refuses to record** when the RTC is unset. On a
switch-powered device that means a user who flips the switch before ever pairing
records nothing, so instead the epoch is persisted alongside the ring metadata,
an estimate is restored on boot, and recording proceeds regardless. Losing audio
is worse than an imprecise start time.

`rtc_valid` in the status read then answers **"can the timestamps on the records
you are about to fetch be trusted?"**, not "is the clock synced right now". The
distinction matters because the app calls `performSyncTime()` on every connect,
so the live clock is authoritative within milliseconds of connecting — while the
backlog waiting to be synced was stamped from the restored estimate and is behind
by however long the switch was off. Reporting the live clock's state would file
that audio in the past. Instead `sd_ring` keeps a watermark at the first record
whose timestamp came from a synced clock, and reports 1 only once `read_seq` has
passed it; before that the app takes its `now - duration` fallback. The watermark
is deliberately not persisted, since a power cycle makes the restored epoch stale
again.

Each record carries its own "was the clock synced when this was stamped" flag
from the audio task to the storage task, and the watermark is planted from that
flag rather than from observing the sync. The two tasks are not ordered against
each other, so deciding at drain time would vouch for a record stamped moments
before the sync landed. The audio task additionally closes the record in progress
whenever the clock is re-anchored, so no record spans the jump.

## Power

There is no button and no wake source, so "off" is the switch and deep sleep
would be a one-way trip. Light sleep is out too: it gates the I2S clock, and the
voice gate needs the mic sampling continuously to catch a speech onset at all.
What is left is CPU frequency scaling, driven by idle time and connection state.

Boot is unattended by design — flipping the switch is a cold boot, so BLE comes
up and starts advertising before the card is mounted, and the card is mounted
from the storage task so a slow, missing, or reformatting card cannot delay it.

## WiFi

**WiFi carries OTA only.** The radio is brought up inside the OTA task and torn
straight back down; neither the app nor the backend has a WiFi ingest path for
device audio, and offline sync is BLE-only by design. Credentials live in RAM, so
they must be re-sent over BLE each session.

HTTPS downloads validate against the Mozilla root CA bundle embedded by
arduino-esp32 and fail closed — an unverifiable certificate aborts the download
rather than downgrading it.

## App compatibility contract

These must be exactly right or a feature silently disables itself rather than
erroring:

- Advertise service UUID `19B10000-E8F2-537E-4F6C-D104768A1214` in the
  **advertising packet**, not just the scan response.
  `native_bluetooth_discoverer.dart` filters on this and nothing else.
- **Omit** the photo characteristics `19B10005` / `19B10006`. The app probes
  `19B10005` and promotes to `DeviceType.openglass` if it reads; absent them the
  device stays `DeviceType.omi` and gets the only connector that implements the
  storage protocol.
- Report DIS firmware revision **`3.0.21`**, purely numeric. `3.0.20+` selects the
  ring protocol; `3.0.17-3.0.19` routes to a multi-file LittleFS protocol and
  anything below to the legacy single-file one. A non-numeric suffix parses as
  `0` and picks the wrong protocol.
- Keep the BLE device name free of the word `glass`: any name containing it
  routes the app to `OmiGlassConnection`, whose storage listener returns null.
- Codec id **21** (`opusFS320`, 320-sample / 20 ms frames).
- Never emit a literal length byte of `0xFF` in a payload — it is a reserved
  escape in the sibling protocol.

## Verification

There is no firmware test harness in this repo, so per the Definition of Done in
the root [`AGENTS.md`](../../../AGENTS.md) verification means exercising the real
path and writing down the evidence.

**Steps 2-11 have not been run — no XIAO ESP32-S3 board was available.** Only
step 1 and the static wire-contract cross-check below have been done, so treat
every runtime claim in this document as designed-and-reviewed, not measured.

1. `pio run` clean build; confirm flash usage leaves both OTA slots viable.
2. Flash, confirm the app discovers the device and streams live audio into a
   conversation.
3. Disconnect, speak for 60+ seconds, reconnect. The synced audio must transcribe
   with a **start time matching when you actually spoke**, not when the sync
   finished. Validates record framing, the codec token, and time sync together.
4. **The duplicate check.** Hold a connected conversation long enough to produce a
   live transcript, then let a sync run. Exactly one conversation must exist for
   that period. This is the core regression test for the single-cursor design and
   must be rerun after any change to `read_seq` handling.
5. **Live audio after a sync.** Once a sync finishes, keep talking on the same
   connection. Audio must keep reaching the live transcript — that is the
   catch-up drain closing the residual backlog every sync leaves behind.
6. **VAD clipping check.** Say a short word after a long silence, several times.
   The first syllable must survive — this is the pre-roll working, and it is the
   failure mode most likely to ship unnoticed.
7. **Gapped timestamp check.** Record three short bursts separated by several
   minutes of silence, then sync. Each must land at its own wall-clock time.
8. **Stale-clock check.** Leave the device off for an hour, switch it on away from
   the phone, speak, then connect and sync. The audio must land at roughly the
   time you spoke, not an hour earlier — that is `rtc_valid` reporting 0 for
   pre-sync records and the app taking its `now - duration` fallback.
9. **Wrap the ring.** Fill the card so the write pointer laps unread data:
   `dropped_packets` increments, a read below `read_seq` returns
   `SEQ_OUT_OF_RANGE` (10), and recording continues.
10. **Pull the switch mid-recording** several times, then power back on and sync.
    The card still mounts, previously written audio is intact, sequence numbers
    survive, and only the final partial record is lost.
11. Run one WiFi OTA cycle.

Cross-check wire bytes against the independent decoder at
[`sdks/rust/omi-device/src/lib.rs`](../../../sdks/rust/omi-device/src/lib.rs) and
the pinned layouts in `app/test/unit/ring_protocol_test.dart` before testing on
hardware — that catches endianness and framing mistakes far faster than the app
will.

# Omi Sync (Expo)

A minimal iOS app that connects to an **Omi DevKit v2** over BLE, downloads the audio sitting on its
SD card — resuming wherever the last session stopped — and plays it back on the phone.

Self-contained: it does not touch `app/`, `backend/`, or the firmware.

## Requirements

- **A physical iPhone.** The iOS Simulator has no Bluetooth LE radio, and Expo Go cannot load native
  BLE modules. You need a development build on real hardware.
- Xcode with an iOS 18 SDK or newer, and Node 20.19+.
- An Omi DevKit v2 with an SD card mounted. The device only advertises once the card mounts.

## Running it

```bash
cd expo_app
npm install
npx expo run:ios --device        # pick your iPhone when prompted
```

Then: **Connect** → accept the iOS pairing prompt → **Sync new audio** → tap a recording to play it.

Nothing connects on its own. The app scans in the background and shows whether the DevKit is in
range; connecting and disconnecting are always your call.

Firmware built with `secure-pairing.conf` keeps its recordings behind a paired link and bonds to one
device at a time — see [Pairing](#pairing) for what the app does when that bond is somewhere else.

## How the device protocol works

Confirmed against [`omi/firmware/devkit/src/storage.c`](../omi/firmware/devkit/src/storage.c) and the
reference client [`omi_sd.py`](../omi/firmware/scripts/devkit/sd_sync/omi_sd.py).

The device advertises `19b10000-…`, but the storage service is **not** in the advertisement — you
discover it after connecting. One characteristic (`30295781-…`) is both the command channel and the
bulk data channel.

Commands are six bytes, and the offset is **big-endian** even though the info characteristic is
little-endian. That inconsistency is the easiest thing to get wrong here:

```
[opcode][segment][off>>24][off>>16][off>>8][off]
opcode: 0=READ  1=DELETE  2=NUKE  3=STOP  50=HEARTBEAT
segment | 0x80  -> read that segment's 16-byte timestamp index instead of its audio
```

Replies are demultiplexed purely by length: a one-byte notification is a status code (`0` ok,
`3` bad segment, `4` empty, `5` past EOF, `6` bad command, `100` end of transfer, `200` deleted).
Anything longer is raw file data. There is no header, sequence number, or checksum.

## Pairing

The firmware can require an encrypted link (`secure-pairing.conf`). In that build everything that
carries content — the recordings, the audio stream, the clock, DFU — is unreachable until the phone
has paired, and the board holds **exactly one bond at a time**.

One characteristic is deliberately exempt: the pairing status, `19b10041-…`, is readable on an
unencrypted link because it exists to explain why pairing failed. The app reads it immediately after
connecting, before anything pairing could block, and it is also how the app knows whether this
particular build encrypts anything at all — older firmware has no pairing service, and the app
treats that as nothing to pair.

There is no CoreBluetooth API to ask iOS to pair. Touching an encrypted characteristic is the
trigger: the device refuses, and iOS raises its prompt in response. So `establishEncryption()`
retries the read that pairing would make succeed until it does, or until the device's 30 s SMP
window closes.

The two ways to be locked out look almost identical from the phone and have opposite remedies, so
the app never guesses — it routes on the reason code the device recorded (`diagnosePairing`):

| Verdict | What happened | Where the user fixes it |
|---|---|---|
| `stale-bond` | The phone holds a key the device threw away (`PinOrKeyMissing`, no bond on the device) | Settings → Bluetooth → Forget This Device |
| `slot-taken` | The single bond belongs to another device | Unpair from that device, or flash the unbond image |
| `needs-pairing` | A slot is free | Accept the iOS prompt |

"Unpair and hand device over" writes `OMIUNBND` to `19b10042-…`, which **erases every recording on
the card** before releasing the bond — the firmware wipes first on purpose, so a new owner cannot
inherit the previous owner's audio. It is behind two confirmations, and the drop that follows is
reported as the stale bond it creates rather than as an unexpected disconnect, because the phone
still holds its half.

## Two things that cause silent data corruption

**Segment numbers are positions, not identities.** They are indices into a ring, so evicting the
oldest segment shifts every other number down. A stored segment number quietly starts pointing at
different audio. Everything here is keyed by the device's stable *sequence* number and converted to
a position only at the moment we talk to the device (`segmentNumberForSeq`).

**The newest segment must not be stored on a partial block.** Resume state is just the size of the
`.bin` on disk. The newest segment is still being recorded, and its length is not a multiple of the
440-byte block size — so if we stored its short tail, the file size would stop being block-aligned,
the next resume would round back down to the previous boundary, and the device would re-send bytes
we already had. `writableLength` truncates to a block boundary for any growing segment and only
keeps a short tail once a segment is sealed. Guarded by
[`src/sync/plan.test.ts`](src/sync/plan.test.ts).

## Audio

The device stores **bare Opus frames**: CELT, 16 kHz mono, 10 ms, ~20 kbps, every payload starting
with TOC byte `0xB0`. Two quirks make naive parsing fail — the firmware writes a stray length byte
at a block boundary when a frame will not fit, and it never clears the tail of its block buffer, so
each block carries stale bytes from the previous one. The parser therefore validates every candidate
against the TOC byte and slides forward one byte at a time when it fails. About 3–4% of the stream
is discarded as resync noise, which is normal.

`react-native-audio-api` decodes Opus through **libopusfile**, which only reads Ogg. So frames are
muxed into an Ogg container in TypeScript before decoding — no WASM, no WebView, no custom native
module.

Segments run from a few megabytes to hundreds, so a segment is listed as ~60-second chunks
(`440 × 325` bytes). Playing one reads just that byte range.

## Layout

```
app/_layout.tsx, app/index.tsx    expo-router, single screen
src/ble/constants.ts              UUIDs, opcodes, SD_BLE_SIZE = 440
src/ble/protocol.ts               pure codec: 21-byte info, 6-byte commands, .idx records
src/ble/pairing.ts                pairing-status codec, and which remedy each failure needs
src/ble/omiDevice.ts              scan, connect, discover, notification stream, pairing
src/ble/useOmiConnection.ts       connection state machine (never auto-connects)
src/sync/plan.ts                  what to fetch, and what is safe to write
src/sync/syncEngine.ts            per-segment READ loop, stall detection, incremental flush
src/sync/segments.ts              manifest shape and its pure transformations
src/sync/progressStore.ts         manifest.json reads and writes
src/sync/fileStore.ts             wires the engine's storage interface to the filesystem
src/audio/opusFrames.ts           TOC-resync frame parser
src/audio/oggOpus.ts              Ogg page writer, CRC-32, OpusHead/OpusTags
src/audio/player.ts               mux -> decodeAudioData -> AudioBufferSourceNode
src/storage/recordings.ts         .bin per segment, chunking, .idx timestamps
scripts/verify-ogg.ts             end-to-end muxer check against ffmpeg
scripts/seed-recording.ts         fixture recording, for testing playback without hardware
```

## Tests

```bash
npm test           # 57 tests
npm run typecheck
npm run verify:ogg # needs ffmpeg on PATH
```

`SyncEngine` takes its BLE client and its storage as interfaces (`SyncClient`, `SyncStore`), so
[`src/sync/syncEngine.test.ts`](src/sync/syncEngine.test.ts) runs the real sync loop under
`node --test` against a fake DevKit that reproduces the wire behaviour that actually bites: blocks
only after a READ, blocks still arriving after a STOP, and a newest segment that grows between
passes.

Those same two interfaces let the engine run against **real hardware from a laptop**, which is how
the BLE path was verified without an iPhone:

```bash
npx tsx scripts/device-sync.ts --dir /tmp/omi-sync --stop 60   # sync, cancel after 60s
npx tsx scripts/device-sync.ts --dir /tmp/omi-sync             # resume
npx tsx scripts/device-sync.ts --dir /tmp/omi-sync --decode    # mux and check with ffmpeg
```

[`scripts/ble-bridge.py`](scripts/ble-bridge.py) stands in for react-native-ble-plx, exposing the
storage characteristic over stdio via bleak; [`scripts/device-sync.ts`](scripts/device-sync.ts)
implements `SyncClient` on top of it and `SyncStore` on the local filesystem. Everything between —
command encoding, the resume plan, block accounting, STOP settling, the frame parser, the muxer —
is the code that runs on the phone. It needs bleak, and defaults to the venv the reference client
already uses (`omi/firmware/scripts/devkit/sd_sync/.venv`).

`verify-ogg` is the important one. It encodes a signal with ffmpeg using the firmware's exact Opus
settings, repacks the packets the way `write_to_storage()` does (stray boundary bytes, stale block
tails), runs them back through the parser and muxer, and decodes the result with ffmpeg. Point it at
a real device dump with `--raw dump.bin` — capture one using
[`pull_range.py`](../omi/firmware/scripts/devkit/sd_sync/pull_range.py).

## Verification evidence

Recorded on 21 Aug 2026, macOS 26.6, Xcode 26.0.1, iPhone 17 Pro simulator; the pairing evidence
on 22 Aug against firmware built with `secure-pairing.conf`.

**Frame parser and Ogg muxer — passing.** `npm run verify:ogg`:

```
1001 reference Opus packets -> 71 blocks of 440 bytes
1001 frames, 1164 bytes skipped (3.7% of the stream), 10.0s of audio
PASS  every original packet recovered byte for byte (1001/1001)
PASS  ffprobe recognises the stream as opus (opus)
PASS  mono (1) / 48 kHz output rate (48000)
PASS  duration matches the frame count (10.01s vs 10.01s)
PASS  decoded audio is sample-identical to the reference (480000 samples compared, max delta 0)
```

Sample-identical to ffmpeg's own decode is the strongest available statement that the container is
correct. The Ogg CRC is checked against a page ffmpeg actually wrote, not against our own
arithmetic.

**Unit tests — passing.** 48 tests over the frame parser (block boundaries, stale tails, shifted
grid, truncated tail), the Ogg muxer (page structure, granule positions, lacing, CRC, determinism),
the wire codec (endianness, sizes above 2³¹, sequence-to-position mapping, eviction), and the sync
planner (resume offsets, block alignment across repeated partial syncs).

**Sync loop against a fake DevKit — passing.** Nine tests drive `SyncEngine.run()` end to end and
assert on the bytes that land in storage: a fresh multi-segment pull is byte-identical to what the
device holds; a sync cancelled mid-transfer leaves a block-aligned file and the next pass reads from
exactly that offset and reconstructs the segment with no duplicated or missing bytes; blocks that
arrive after a STOP are discarded instead of corrupting the next segment; the growing newest
segment's partial block is held back and picked up once the segment seals; a rotated-past segment is
marked evicted and that mark is persisted; the `.idx` sidecar is fetched once and never re-fetched,
and one rejection stops the engine asking for the rest of the run. This is the closest substitute
for hardware, and it covers the resume path that the on-device run is meant to confirm.

**Build and UI — passing.** `npx expo run:ios` builds clean and launches. On the simulator the app
correctly reports that there is no Bluetooth LE radio and disables Connect.

**Playback on iOS — passing.** A fixture recording was seeded with `scripts/seed-recording.ts` (90 s
of audio, 221,760 bytes, 504 blocks) and the full path exercised in the simulator: byte-range read →
TOC resync → Ogg mux → `decodeAudioData` → `AudioBufferSourceNode`. It produced two chunks with
wall-clock labels from the `.idx` records, decoded to a 58.21 s buffer at 48 kHz, played to its
natural end, and reported an accurate running position throughout. Playback was triggered by a
temporary auto-play call (identical to what the row tap calls) because the host machine denies the
accessibility permission needed to script taps into the simulator.

**Sync against a real DevKit v2 — passing.** Run over BLE from the laptop through
`scripts/device-sync.ts`, against a device holding 3 segments (sequence 13–15, 134 MB):

```
Connected to 767AD1FF-EB82-B418-FB71-404DC1C245E4, ATT MTU 498
  3 segments on card, sequence 13..15, newest holds 24.0 MB, 128.0 MB retained
  [pulling] Pulling segment 15 1007.6 KB/24.6 MB at 16.8 KB/s
  --- cancelling after 60s ---
Result: 1015.4 KB pulled, cancelled=true
  seq 15: 1015.4 KB on disk (block-aligned)
```

Then a second pass, with no state but the files on disk:

```
  on disk before: 15:1039720
      -> cmd 0 segment 3 offset 1039720
Result: 757.1 KB pulled, cancelled=true
  seq 15: 1.7 MB on disk (block-aligned)
```

- **Resume asks for exactly the right byte.** The second pass requested offset 1,039,720, which is
  the size of the file the first pass left, and a multiple of 440.
- **Nothing was duplicated or lost.** The first pass's 1,039,720 bytes are unchanged byte for byte
  after the second pass.
- **The bytes are the device's bytes.** Pulling the same ranges with the reference Python client and
  comparing: offset 499,840 (before the seam) and offset 1,199,880 (160 KB *after* the seam) are
  both byte-identical over 44,000 bytes. A wrong resume offset would have shifted everything after
  the seam.
- Throughput held at 16.8 KB/s, above the 14–15 KB/s the reference client sees.

A third pass then ran the segment to completion — 23.7 MB in 24 minutes at a steady 16.9 KB/s,
across three sessions in total:

```
Result: 23.7 MB pulled, cancelled=false
  seq 15: 25.5 MB on disk (block-aligned), complete
```

- 26,690,400 bytes on disk: exactly 60,660 blocks, no remainder.
- Byte-identical to the reference client at offset 25,999,600 as well, so every one of those blocks
  landed at the right absolute position, not just the early ones.
- Decodes to 1,152,652 frames = 3 h 12 m of audio at a 3.2% resync-skip rate; `ffprobe` reports
  `opus / 48000 / 1 / 11526.520000`, matching the frame count exactly, mean level −44.3 dB.
- The device then **crashed on the optional `.idx` fetch** that follows a completed segment (see
  below). The engine treated the dead link as a stall, stopped asking for indexes, and finished
  reporting `Synced 23.7 MB` with the audio and the manifest intact — the failure cost only the
  timestamps.

**Pairing against the secure build — passing.** The DevKit was reflashed with `secure-pairing.conf`
and the Mac holds its single bond, so the app's own `parsePairingStatus` and `diagnosePairing` ran
against the real 25 bytes through the same bridge:

```
$ npx tsx scripts/device-sync.ts --dir /tmp/pairing-check
Connected to 767AD1FF-…, ATT MTU 498
  pairing: ready (SMP on, 1/1 slots, link encrypted)
  1 segments on card, sequence 2..2, newest holds 11.8 MB

Result: 13.0 MB pulled, cancelled=false
  seq 2: 13.0 MB on disk (block-aligned), complete, 210 index records
```

Cross-checked against the reference client, which reports the same state
(`python3 pairing.py` → `SMP on, bondable yes, persistent bonds yes / 1 of 1 slots used / security
level 2`).

A partial pull was interrupted first, so this run also re-exercised resume; it finished
block-aligned at a steady 15.7 KB/s over 14 minutes, unaffected by the encrypted link. **The `.idx`
sidecar fetched cleanly for the first time** — 210 records — which is the path that used to reset
the device. `--decode` then produced 546,100 frames (2.9% resync skips) = 5461.0 s, matching
`ffprobe`'s `opus / 48000 / 1 / 5461.000000` exactly, mean level −27.4 dB.

**Pairing UI — rendered, not exercised.** The simulator has no BLE, so the three new states were
forced into the screen with a temporary override, screenshotted, and the override removed: the
paired row (`Paired · 1 of 1 slot`) with the unpair action, the stale-bond remedy, and the
"waiting for pairing" spinner.

**Not verified: pairing from iOS itself, the phone's BLE stack, and the UI.** Only the `ready`
verdict could be produced on real hardware — reaching `needs-pairing`, `stale-bond` or `slot-taken`
means releasing the bond this Mac holds, which erases the card and leaves the Mac unable to re-pair
without forgetting the device. Nothing has exercised CoreBluetooth's pairing prompt, which is the
one step `establishEncryption()` exists for. No iPhone was paired to this machine
(`xcrun devicectl list devices` → no devices found), so `react-native-ble-plx` itself, scanning,
the availability timeout, the Connect/Disconnect buttons and the on-device sync UI have still never
run. The protocol, the resume logic and the audio path above them now have.

**Still not exercised on hardware: sealed segments.** The `.idx` sidecar and sealed segments were
both blocked by the firmware reset described below, which has since been fixed; the sidecar now
works (above). The card was wiped when the secure image was flashed and holds a single growing
segment, so there is still no sealed segment to read — that needs a card that has rotated at least
once (`ring-fast-rotate.conf`).

To finish the job, plug in an iPhone, run `npx expo run:ios --device`, and work through:

1. Power the DevKit on and off, and confirm availability goes stale after ~10 s.
2. Connect and accept the pairing prompt; confirm the Device card then reads `Paired · 1 of 1 slot`.
3. Connect with the bond held by another device and confirm the app says the slot is taken rather
   than reporting a generic encryption error.
4. Unpair from the app, then reconnect without forgetting the device in Settings — the app should
   report the stale bond, and connecting should work once iOS has forgotten it.
5. Sync, and confirm the throughput reads around 14–17 KB/s.
6. Force-quit mid-transfer, reopen, and confirm the resume offset matches the stored byte count.
7. Disconnect during a sync and confirm the device is not left with an open read handle.
8. Play a synced chunk and confirm it sounds like the room, not like noise.

## Firmware defect found while testing — since fixed

The DevKit reset when asked to read **any segment other than the one currently being recorded**,
and when asked for **any timestamp index**. Reproducible with the reference Python client, so it was
not an app bug:

```
$ python3 pull_range.py 0 44000 out.wav 1     # sealed segment
bleak.exc.BleakError: disconnected
$ python3 pull_range.py 25000000 44000 out.wav 3   # newest segment
  downloaded 44,000 bytes in 4s = 10.7 KB/s        # works
```

The device really is restarting, not just dropping the link: the `sync errors` counter in the
storage-info characteristic goes 37 → 2 across one failed read, while it climbs monotonically
(10 → 18 → 27) when the device is left alone.

The cause was that `parse_storage_command()` runs in the GATT write callback and, for those two
cases only, called `get_file_size()` / `index_get_size_for()` — both of which take `sd_mutex` and
then `fs_stat()` the card. Reading the newest segment is the one path that answers from the
in-memory `audio_file_size` counter without touching the filesystem, and it was the one path that
worked. A FatFs directory walk does not fit the BT RX thread's 1 KB stack.

Fixed in the firmware: `parse_storage_command()` now makes no filesystem call at all, and
`setup_storage_tx()` decides the size on the storage thread, reporting an empty or exhausted target
from there. The status still arrives as a notification, just a little later than the command ACK,
which the engine already handled. The app needed no change.

## Expo SDK version

This project is on **Expo SDK 54**, not 57. SDK 57 requires **Xcode 26.4+**; its `expo-modules-jsi`
uses `weak let`, which needs Swift 6.3 and fails to compile on Xcode 26.0.1 with
`'weak' must be a mutable variable, because it may change at runtime`. SDK 54 is the newest SDK the
installed toolchain can build.

Nothing in this app depends on an SDK 55+ API — the `File`/`Directory`/`FileHandle` filesystem API
used for resume is identical in both. After upgrading Xcode, `npx expo install expo@^57 --fix` should
be enough.

## Notes

- iOS negotiates the ATT MTU automatically, which covers the 440-byte notifications. The connected
  value is shown in the UI and flagged if it drops below 443. **Android would need
  `requestMTU(512)`** before any transfer, or the firmware retries the same block forever with no
  error.
- The firmware sleeps ~1.5 s between accepting a READ and sending the first block, so the app allows
  extra grace before the first byte and only then applies a 10 s stall timeout.
- `babel.config.js` exists solely to add the worklets plugin, which `react-native-reanimated` needs.
  Reanimated and `react-native-gesture-handler` are undeclared dependencies of
  `react-native-audio-api`'s exported playback controls; without them the bundle fails to resolve.

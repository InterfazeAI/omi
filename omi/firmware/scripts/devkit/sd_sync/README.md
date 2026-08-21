# SD Sync Tools

Host tools for pulling offline recordings off a DevKit v2 over BLE and decoding them.

These replace the hardcoded-device-ID scripts in the parent directory: they discover the
device by service UUID, and their frame parser resynchronises on the Opus TOC byte instead of
assuming the 440-byte block grid is aligned. That last point matters — a decoder that trusts
alignment produces plausible-looking garbage rather than an obvious failure.

Background, on-card format and the traps behind all of this:
[`omi/firmware/devkit/DEBUGGING.md`](../../../devkit/DEBUGGING.md).

## Setup

```bash
python3 -m venv .venv && source .venv/bin/activate
pip install -r ../requirements.txt
brew install opus          # macOS; the decoder loads libopus directly
```

## Segments

Recording is a ring of segment files, not one growing file: the firmware never stops recording,
and when the card budget is reached it unlinks the oldest segment. Two consequences for these
tools:

- **Offsets are within a segment**, not across the whole recording.
- **Segment numbers are positions, not identities.** They run 1 (oldest) to `count` (the one
  being recorded), so an eviction shifts every number down by one. To follow a particular
  segment over time, use the sequence numbers `info.py` prints.

Every script defaults to the segment being recorded into, which is what you want for "pull back
what I just said".

**Time is measured in recorded audio, not wall clock.** Each segment has a `.idx` sidecar holding
a mark every 30 s, and `pull_last.py` walks it backwards to turn minutes into a byte offset. Marks
carry uptime rather than a date — the clock is only meaningful once an app has set it — so a
reboot shows up as uptime restarting. That costs no audio, since the device simply appends to the
same segment when it comes back, and the walk treats the boundary as a zero-length gap.

## Usage

Run from this directory (the scripts import `omi_sd` as a sibling).

```bash
# Segments on the card, retention, and how long a full sync would take
python3 info.py

# Pull back the last N minutes of recorded audio, joining segments if the window
# crosses a rotation. Uses the on-card timestamp index, so it stays correct
# under VBR, where a quiet hour occupies far fewer bytes than a loud one.
python3 pull_last.py 30 ~/Desktop/last30.wav

# Record while you speak, then pull back exactly that span
# (handles the ring rotating mid-window by fetching each part and joining them)
python3 record_and_pull.py 60 ~/Desktop/take1.wav

# Re-decode a span already on the card, without recording again.
# Trailing argument picks a segment; omit it for the one being recorded.
python3 pull_range.py 6164808 142560 ~/Desktop/take1.wav 3

# Check sync speed (healthy is ~14 KB/s)
python3 throughput.py 25
```

## Reading the output

```
frames 6,009  decode errors 0  resync skips 3,835
duration 60.1s   RMS -20.2 dBFS   peak 0.0 dBFS
```

- **decode errors** should be 0. Anything else means the parser lost the frame boundaries.
- **resync skips** are expected and not an error: they count the stray length bytes at block
  boundaries plus any grid shift. A few percent of the byte count is normal.
- **duration** should match the window you asked for. Substantially short means frames were
  dropped or misparsed.
- **peak 0.0 dBFS** means the mic clipped during capture. That distortion is baked in and
  cannot be undone by decoding; lower `MIC_GAIN` in `devkit/src/config.h`.

The device only advertises after its SD card mounts, so "device not found" usually means a
card problem rather than a radio problem.

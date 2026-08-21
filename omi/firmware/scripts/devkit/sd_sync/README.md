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

## Usage

Run from this directory (the scripts import `omi_sd` as a sibling).

```bash
# Record while you speak, then pull back exactly that span
python3 record_and_pull.py 60 ~/Desktop/take1.wav

# Re-decode a span already on the card, without recording again
python3 pull_range.py 6164808 142560 ~/Desktop/take1.wav

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

"""Shared pieces for pulling offline recordings off a DevKit v2 over BLE.

Format and protocol notes live in omi/firmware/devkit/DEBUGGING.md; the frame parser here is
the part most worth reading, because a decoder that trusts block alignment will silently
produce garbage.
"""
import asyncio
import ctypes
import ctypes.util
import struct
import time
import wave

import numpy as np
from bleak import BleakClient, BleakScanner

AUDIO_SVC = "19b10000-e8f2-537e-4f6c-d104768a1214"
CMD_CHAR = "30295781-4301-eabd-2904-2849adfeae43"
SIZE_CHAR = "30295782-4301-eabd-2904-2849adfeae43"

READ_COMMAND, DELETE_COMMAND, NUKE_COMMAND, STOP_COMMAND = 0, 1, 2, 3
END_OF_TRANSFER = 100

SD_BLE_SIZE = 440
RATE = 16000
FRAME_SAMPLES = 160

# Every Opus payload starts with this TOC byte while the encoder config is fixed (CELT, 16kHz,
# 10ms frames). It is what makes resynchronising possible.
OPUS_TOC = 0xB0


def _load_opus():
    for name in ("/opt/homebrew/lib/libopus.dylib", "/usr/local/lib/libopus.dylib",
                 ctypes.util.find_library("opus")):
        if not name:
            continue
        try:
            return ctypes.CDLL(name)
        except OSError:
            continue
    raise RuntimeError("libopus not found - install it (macOS: brew install opus)")


_opus = _load_opus()
_opus.opus_decoder_create.restype = ctypes.c_void_p
_opus.opus_decoder_create.argtypes = [ctypes.c_int32, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
_opus.opus_decode.restype = ctypes.c_int
_opus.opus_decode.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int32,
                              ctypes.POINTER(ctypes.c_int16), ctypes.c_int, ctypes.c_int]


async def find_device(timeout=120):
    """Scan until the recorder shows up. It only advertises once the SD card has mounted."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        found = await BleakScanner.discover(timeout=6.0, return_adv=True)
        for _, (dev, adv) in found.items():
            if AUDIO_SVC in [u.lower() for u in (adv.service_uuids or [])]:
                return dev
        print("  ...rescanning", flush=True)
    return None


async def read_size(client):
    """Returns (recording length, saved read offset)."""
    return struct.unpack("<II", (await client.read_gatt_char(SIZE_CHAR))[:8])


def _command(cmd, offset=0, file_no=1):
    return bytes([cmd, file_no,
                  (offset >> 24) & 0xFF, (offset >> 16) & 0xFF,
                  (offset >> 8) & 0xFF, offset & 0xFF])


async def download(client, start, length, stall_limit=10):
    """Pulls `length` bytes from `start`. Returns (data, seconds elapsed).

    `start` is rounded down to a block boundary: the firmware serves 440-byte blocks and a
    mid-block start makes every following frame unparseable.
    """
    start -= start % SD_BLE_SIZE
    chunks, done = [], asyncio.Event()

    def on_notify(_, data):
        payload = bytes(data)
        if len(payload) == 1:
            if payload[0] == END_OF_TRANSFER:
                done.set()
        else:
            chunks.append(payload)

    await client.start_notify(CMD_CHAR, on_notify)
    await client.write_gatt_char(CMD_CHAR, _command(READ_COMMAND, start), response=True)

    seen, stalled, began = 0, 0, time.time()
    while not done.is_set():
        await asyncio.sleep(1.0)
        got = sum(len(c) for c in chunks)
        stalled = stalled + 1 if got == seen else 0
        seen = got
        if got >= length or stalled >= stall_limit:
            break
    elapsed = time.time() - began

    try:
        await client.write_gatt_char(CMD_CHAR, _command(STOP_COMMAND), response=True)
        await client.stop_notify(CMD_CHAR)
    except Exception:
        pass

    return b"".join(chunks)[:length], elapsed


def parse_frames(raw):
    """Splits the raw byte stream into Opus frames. Returns (frames, bytes skipped).

    Frames are [length][payload], but two things stop a straight walk from working: each
    440-byte block boundary carries a stray length byte with no payload, and the block grid
    itself can be shifted if any write in the file's history was short. So validate every
    candidate against the known TOC byte and slide forward a byte at a time when it fails.
    """
    frames, skipped, i = [], 0, 0
    while i + 1 < len(raw):
        length = raw[i]
        if raw[i + 1] == OPUS_TOC and 2 <= length <= 60 and i + 1 + length <= len(raw):
            frames.append(raw[i + 1:i + 1 + length])
            i += 1 + length
        else:
            skipped += 1
            i += 1
    return frames, skipped


def decode(frames):
    """Decodes frames to mono int16 PCM. Returns (samples, frames that failed)."""
    err = ctypes.c_int()
    state = _opus.opus_decoder_create(RATE, 1, ctypes.byref(err))
    buf = (ctypes.c_int16 * (FRAME_SAMPLES * 8))()
    pcm, bad = [], 0
    for frame in frames:
        n = _opus.opus_decode(state, frame, len(frame), buf, FRAME_SAMPLES * 8, 0)
        if n > 0:
            pcm.append(np.frombuffer(bytes(buf)[: n * 2], dtype=np.int16))
        else:
            bad += 1
    return (np.concatenate(pcm) if pcm else np.array([], dtype=np.int16)), bad


def write_wav(path, samples):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(samples.tobytes())


def save_and_report(raw, elapsed, out_path):
    """Parse, decode, write and print the usual summary. Returns True if anything decoded."""
    print(f"  downloaded {len(raw):,} bytes in {elapsed:.0f}s = "
          f"{len(raw)/max(elapsed,1)/1024:.1f} KB/s", flush=True)

    frames, skipped = parse_frames(raw)
    samples, bad = decode(frames)
    if len(samples) == 0:
        print("  DECODE FAILED - no frames recovered")
        return False

    write_wav(out_path, samples)
    scaled = samples.astype(np.float64) / 32768.0
    rms = float(np.sqrt(np.mean(scaled ** 2)))
    peak = float(np.max(np.abs(scaled)))
    print(f"  frames {len(frames):,}  decode errors {bad}  resync skips {skipped}")
    print(f"  duration {len(samples)/RATE:.1f}s   "
          f"RMS {20*np.log10(rms+1e-12):.1f} dBFS   peak {20*np.log10(peak+1e-12):.1f} dBFS")
    if peak >= 0.999:
        print("  NOTE: clipping at full scale - lower MIC_GAIN in config.h")
    print(f"  WAV -> {out_path}")
    return True

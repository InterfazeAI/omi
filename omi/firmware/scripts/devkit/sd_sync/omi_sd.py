"""Shared pieces for pulling offline recordings off a DevKit v2 over BLE.

Format and protocol notes live in omi/firmware/devkit/DEBUGGING.md; the frame parser here is
the part most worth reading, because a decoder that trusts block alignment will silently
produce garbage.
"""
import asyncio
import ctypes
import ctypes.util
import struct
import sys
import time
import wave

import numpy as np
from bleak import BleakClient, BleakScanner

AUDIO_SVC = "19b10000-e8f2-537e-4f6c-d104768a1214"
CMD_CHAR = "30295781-4301-eabd-2904-2849adfeae43"
SIZE_CHAR = "30295782-4301-eabd-2904-2849adfeae43"

# Readable without pairing, by design -- it explains why pairing did not work, so requiring
# pairing to read it would defeat the point. Publishes counters only, never content.
PAIRING_STATUS_CHAR = "19b10041-e8f2-537e-4f6c-d104768a1214"
PAIRING_RELEASE_CHAR = "19b10042-e8f2-537e-4f6c-d104768a1214"
UNBOND_MAGIC = b"OMIUNBND"

READ_COMMAND, DELETE_COMMAND, NUKE_COMMAND, STOP_COMMAND = 0, 1, 2, 3
END_OF_TRANSFER = 100

# Set on a segment number to address that segment's timestamp index instead of its audio.
SEGMENT_INDEX_FLAG = 0x80

SD_BLE_SIZE = 440
RATE = 16000
FRAME_SAMPLES = 160

# Ask for more than any file holds to mean "read to the end"; the firmware ends the transfer
# itself. download() recognises it and reports progress without a percentage, since the total
# is unknown until the transfer stops.
READ_TO_EOF = 1 << 26

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


def format_size(count):
    return f"{count/1_048_576:.1f} MB" if count >= 1_048_576 else f"{count/1024:.0f} KB"


def format_duration(seconds):
    seconds = int(max(seconds, 0))
    if seconds < 60:
        return f"{seconds}s"
    if seconds < 3600:
        return f"{seconds // 60}m{seconds % 60:02d}s"
    return f"{seconds // 3600}h{(seconds % 3600) // 60:02d}m"


class Progress:
    """Live transfer progress.

    A sync of any length runs for minutes at ~15 KB/s, and printing nothing until it finished
    was indistinguishable from a hang. Rewrites a single line on a terminal; when the output is
    a file or a pipe it prints far less often, so logs stay readable.
    """

    def __init__(self, label, total):
        self.label = label
        self.total = total
        self.terminal = sys.stdout.isatty()
        self.interval = 2.0 if self.terminal else 15.0
        self.started = time.time()
        self.marked_at = self.started
        self.marked_bytes = 0
        # Wait a full interval before the first line, so a transfer that finishes quickly —
        # an index is a couple of KB — reports once rather than twice.
        self.printed_at = self.started

    def update(self, done):
        now = time.time()
        if now - self.printed_at < self.interval:
            return
        window = now - self.marked_at
        rate = (done - self.marked_bytes) / window if window > 0 else 0.0
        self.marked_at, self.marked_bytes, self.printed_at = now, done, now
        self._render(done, rate, now - self.started, final=False)

    def finish(self, done):
        elapsed = time.time() - self.started
        self._render(done, done / elapsed if elapsed > 0 else 0.0, elapsed, final=True)
        return elapsed

    def _render(self, done, rate, elapsed, final):
        fields = [self.label]
        if self.total:
            # The last notification can carry past the requested length; the extra is trimmed
            # off the saved data, so reporting over 100% would only look like a fault.
            done = min(done, self.total)
            fields.append(f"{100 * done / self.total:4.0f}%")
            fields.append(f"{format_size(done)} of {format_size(self.total)}")
        else:
            fields.append(format_size(done))
        fields.append(f"{rate/1024:.1f} KB/s")
        if final:
            fields.append(f"in {format_duration(elapsed)}")
        else:
            if self.total and rate > 0:
                fields.append(f"eta {format_duration((self.total - done) / rate)}")
            fields.append(format_duration(elapsed))

        line = "  " + "   ".join(fields)
        if self.terminal:
            print("\r" + line.ljust(76), end="\n" if final else "", flush=True)
        else:
            print(line, flush=True)


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


class RingInfo:
    """What the storage-info characteristic reports about the recording ring.

    Recording is a ring of segment files. `count` of them are on the card; they are addressed
    by a 1-based number, oldest first, so `count` is always the one being recorded into.
    Deleting a segment shifts the numbers above it down by one.
    """

    def __init__(self, raw):
        self.newest_bytes, self.saved_offset = struct.unpack("<II", raw[:8])
        if len(raw) >= 21:
            self.count = raw[8]
            self.oldest_seq, self.newest_seq, self.segment_bytes = struct.unpack("<III", raw[9:21])
        else:
            # Pre-ring firmware: a single file that grows until it stops.
            self.count, self.oldest_seq, self.newest_seq = 1, 1, 1
            self.segment_bytes = 0

        if len(raw) >= 34:
            self.max_count = raw[21]
            self.evictions, last_err, self.sync_errors = struct.unpack("<IiI", raw[22:34])
            self.last_evict_err = last_err
        else:
            self.max_count = 0
            self.evictions = self.sync_errors = self.last_evict_err = 0

        # A card that stops accepting the append looks exactly like a dead microphone from here:
        # newest_bytes simply stops moving. These name the failing operation and its errno.
        if len(raw) >= 50:
            (self.open_failures, self.last_open_err,
             self.write_failures, self.last_write_err) = struct.unpack("<IiIi", raw[34:50])
        else:
            self.open_failures = self.write_failures = 0
            self.last_open_err = self.last_write_err = 0

    @property
    def io_healthy(self):
        return not (self.open_failures or self.write_failures)

    @property
    def total_bytes(self):
        """Approximate total across the ring; only the newest segment's length is exact."""
        return (self.count - 1) * self.segment_bytes + self.newest_bytes

    def __str__(self):
        return (f"{self.count} segments (seq {self.oldest_seq}..{self.newest_seq}), "
                f"{self.segment_bytes:,} B each, newest holds {self.newest_bytes:,} B")


class PairingStatus:
    """Why pairing did or did not work, straight from the device.

    Serial logging is not reliable on this board (DEBUGGING.md trap 8) and the SMP debug image
    does not boot, so this characteristic is the only way to see the reason code the Bluetooth
    stack produced. It is deliberately readable on an unencrypted link.
    """

    # bt_security_err, from zephyr/include/zephyr/bluetooth/conn.h
    SECURITY_ERR = {
        0: "success",
        1: "authentication failed",
        2: "PIN or key missing -- the peer offered a key this device no longer has",
        3: "OOB data not available",
        4: "authentication requirements not met",
        5: "pairing not supported",
        6: "pairing not allowed -- typically no free bond slot",
        7: "invalid parameters",
        8: "key rejected",
        9: "unspecified",
    }

    def __init__(self, raw):
        if len(raw) < 25:
            raise ValueError(f"pairing status too short: {len(raw)} bytes")
        self.version = raw[0]
        flags = raw[1]
        self.smp_enabled = bool(flags & 0x01)
        self.bondable = bool(flags & 0x02)
        self.settings_enabled = bool(flags & 0x04)
        self.link_encrypted = bool(flags & 0x08)
        self.bond_count = raw[2]
        self.max_bonds = raw[3]
        self.last_pairing_err = raw[4]
        self.last_security_err = raw[5]
        self.last_security_level = raw[6]
        self.current_security_level = raw[7]
        (self.connections, self.pairing_successes,
         self.pairing_failures, self.unbond_requests) = struct.unpack("<IIII", raw[8:24])
        self.last_unbond_result = struct.unpack("<b", raw[24:25])[0]

    @property
    def slots_full(self):
        return self.bond_count >= self.max_bonds

    def describe_pairing_err(self):
        return self.SECURITY_ERR.get(self.last_pairing_err, f"unknown ({self.last_pairing_err})")

    def describe_security_err(self):
        return self.SECURITY_ERR.get(self.last_security_err, f"unknown ({self.last_security_err})")


async def read_pairing_status(client):
    return PairingStatus(await client.read_gatt_char(PAIRING_STATUS_CHAR))


async def release_bond(client):
    """Hand the device to a new owner. Needs the encrypted link.

    DESTRUCTIVE: the device erases every recording before releasing the bond, so the next device
    to pair cannot read the previous owner's audio. Pull anything worth keeping first.
    """
    await client.write_gatt_char(PAIRING_RELEASE_CHAR, UNBOND_MAGIC, response=True)


async def read_info(client):
    return RingInfo(await client.read_gatt_char(SIZE_CHAR))


async def read_size(client):
    """Returns (length of the segment being recorded, saved read offset)."""
    info = await read_info(client)
    return info.newest_bytes, info.saved_offset


def _command(cmd, offset=0, file_no=1):
    return bytes([cmd, file_no,
                  (offset >> 24) & 0xFF, (offset >> 16) & 0xFF,
                  (offset >> 8) & 0xFF, offset & 0xFF])


async def download(client, start, length, segment=None, stall_limit=10, want_index=False, label=None):
    """Pulls `length` bytes from `start` within `segment`. Returns (data, seconds elapsed).

    `segment` defaults to the one currently being recorded into. `start` is rounded down to a
    block boundary: the firmware serves 440-byte blocks and a mid-block start makes every
    following frame unparseable. Pass READ_TO_EOF as `length` to take the rest of the file.
    """
    if segment is None:
        segment = (await read_info(client)).count
    file_no = segment | SEGMENT_INDEX_FLAG if want_index else segment

    start -= start % SD_BLE_SIZE
    chunks, done = [], asyncio.Event()
    if label is None:
        label = f"{'index' if want_index else 'audio'} seg {segment}"
    progress = Progress(label, None if length >= READ_TO_EOF else length)

    def on_notify(_, data):
        payload = bytes(data)
        if len(payload) == 1:
            if payload[0] == END_OF_TRANSFER:
                done.set()
        else:
            chunks.append(payload)

    await client.start_notify(CMD_CHAR, on_notify)
    await client.write_gatt_char(CMD_CHAR, _command(READ_COMMAND, start, file_no), response=True)

    seen, stalled = 0, 0
    while not done.is_set():
        await asyncio.sleep(1.0)
        got = sum(len(c) for c in chunks)
        stalled = stalled + 1 if got == seen else 0
        seen = got
        progress.update(got)
        if got >= length or stalled >= stall_limit:
            break
    elapsed = progress.finish(min(seen, length))
    if stalled >= stall_limit:
        print(f"  stalled for {stall_limit}s with no data; ending this transfer early")

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
    """Parse, decode, write and print the usual summary.

    Returns the decoded duration in seconds, or None if nothing decoded — so callers can both
    test it for success and use it to place the audio in time.
    """
    print(f"  transferred {format_size(len(raw))} in {format_duration(elapsed)} "
          f"({len(raw)/max(elapsed, 1)/1024:.1f} KB/s average)", flush=True)

    began = time.time()
    frames, skipped = parse_frames(raw)
    samples, bad = decode(frames)
    if len(samples) == 0:
        print("  DECODE FAILED - no frames recovered")
        return None
    print(f"  decoded in {format_duration(time.time() - began)}")

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
    return len(samples) / RATE

"""Shared pieces for pulling offline recordings off a DevKit v2 over BLE.

Format and protocol notes live in omi/firmware/devkit/DEBUGGING.md; the frame parser here is
the part most worth reading, because a decoder that trusts block alignment will silently
produce garbage.
"""
import asyncio
import ctypes
import ctypes.util
import math
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

    `bar` draws a filled bar and is meant for a whole job rather than one transfer. `resumed`
    is byte count carried over from an earlier run: it counts towards the percentage but not
    towards the rate, so an interrupted-and-restarted pull does not report an absurd opening
    speed from bytes it never actually transferred.
    """

    BAR_WIDTH = 22

    def __init__(self, label, total, bar=False, resumed=0):
        self.label = label
        self.total = total
        self.bar = bar
        self.resumed = resumed
        self.terminal = sys.stdout.isatty()
        self.interval = 2.0 if self.terminal else 15.0
        self.started = time.time()
        self.marked_at = self.started
        self.marked_bytes = resumed
        # Wait a full interval before the first line, so a transfer that finishes quickly —
        # an index is a couple of KB — reports once rather than twice.
        self.printed_at = self.started
        # Smoothed so the eta does not lurch on a momentary stall; BLE throughput here is steady
        # enough that a jumpy estimate is noise rather than information.
        self.rate = 0.0

    def update(self, done):
        now = time.time()
        if now - self.printed_at < self.interval:
            return
        window = now - self.marked_at
        if window > 0:
            sample = (done - self.marked_bytes) / window
            self.rate = sample if self.rate == 0.0 else 0.7 * self.rate + 0.3 * sample
        self.marked_at, self.marked_bytes, self.printed_at = now, done, now
        self._render(done, self.rate, now - self.started, final=False)

    def finish(self, done):
        elapsed = time.time() - self.started
        moved = max(done - self.resumed, 0)
        self._render(done, moved / elapsed if elapsed > 0 else 0.0, elapsed, final=True)
        return elapsed

    def _render(self, done, rate, elapsed, final):
        # The last notification can carry past the requested length; the extra is trimmed
        # off the saved data, so reporting over 100% would only look like a fault.
        if self.total:
            done = min(done, self.total)

        fields = [self.label] if self.label else []
        if self.total and self.bar:
            filled = int(self.BAR_WIDTH * done / self.total)
            fields.append("[" + "#" * filled + "." * (self.BAR_WIDTH - filled) + "]")
        if self.total:
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
            if not self.bar:
                fields.append(format_duration(elapsed))

        line = "  " + "   ".join(fields)
        if self.terminal:
            print("\r" + line.ljust(88), end="\n" if final else "", flush=True)
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


async def _transfer(client, start, length, file_no, stall_limit, on_drain, on_progress):
    """Core read loop, shared by the in-memory and file-backed downloads.

    Calls `on_drain(chunks)` about once a second with whatever has arrived since the last call,
    then `on_progress(total)`. Returns (bytes seen, seconds elapsed, why it stopped), where the
    reason is one of:

        "eof"    the firmware signalled end of file, so the read is complete
        "length" the requested number of bytes arrived
        "stall"  nothing arrived for `stall_limit` seconds

    Only "eof" proves a whole file was read, which is what makes a resumed pull able to tell a
    finished segment from one that was cut short.
    """
    start -= start % SD_BLE_SIZE
    pending, ended = [], asyncio.Event()

    def on_notify(_, data):
        payload = bytes(data)
        if len(payload) == 1:
            if payload[0] == END_OF_TRANSFER:
                ended.set()
        else:
            pending.append(payload)

    await client.start_notify(CMD_CHAR, on_notify)
    await client.write_gatt_char(CMD_CHAR, _command(READ_COMMAND, start, file_no), response=True)

    began = time.time()
    seen, stalled, reason = 0, 0, "stall"
    try:
        while True:
            await asyncio.sleep(1.0)
            # Take what has arrived before testing for the end, or the chunks delivered in the
            # same second as the end-of-transfer marker are dropped -- which truncates every
            # segment by up to a second of audio.
            taking = len(pending)
            batch = pending[:taking]
            del pending[:taking]

            added = sum(len(c) for c in batch)
            if batch:
                on_drain(batch)
            seen += added
            stalled = 0 if added else stalled + 1
            on_progress(seen)

            if ended.is_set():
                reason = "eof"
                break
            if seen >= length:
                reason = "length"
                break
            if stalled >= stall_limit:
                reason = "stall"
                break
    finally:
        try:
            await client.write_gatt_char(CMD_CHAR, _command(STOP_COMMAND), response=True)
            await client.stop_notify(CMD_CHAR)
        except Exception:
            pass

    return seen, time.time() - began, reason


async def download(client, start, length, segment=None, stall_limit=10, want_index=False, label=None):
    """Pulls `length` bytes from `start` within `segment`. Returns (data, seconds elapsed).

    `segment` defaults to the one currently being recorded into. `start` is rounded down to a
    block boundary: the firmware serves 440-byte blocks and a mid-block start makes every
    following frame unparseable. Pass READ_TO_EOF as `length` to take the rest of the file.

    Holds everything in memory. Use download_to_file() for a whole-card pull, where that is
    both large and lost on interruption.
    """
    if segment is None:
        segment = (await read_info(client)).count
    file_no = segment | SEGMENT_INDEX_FLAG if want_index else segment
    if label is None:
        label = f"{'index' if want_index else 'audio'} seg {segment}"

    chunks = []
    progress = Progress(label, None if length >= READ_TO_EOF else length)
    seen, _, reason = await _transfer(client, start, length, file_no, stall_limit,
                                      chunks.extend, progress.update)
    elapsed = progress.finish(min(seen, length))
    if reason == "stall":
        print(f"  stalled for {stall_limit}s with no data; ending this transfer early")

    return b"".join(chunks)[:length], elapsed


async def download_to_file(client, start, length, path, segment, stall_limit=10, on_progress=None):
    """Append a segment to `path` as it arrives. Returns (bytes appended, elapsed, reason).

    Written straight to disk roughly once a second rather than accumulated in memory, so an
    interrupted pull keeps everything up to the last second and can resume from the file's own
    length. A whole-card pull is hours long and gigabytes large; losing it to a dropped
    connection is not acceptable, and holding it in RAM is not possible.

    The caller must pass a `start` that is already a multiple of SD_BLE_SIZE and matches what is
    on disk. The firmware rounds a mid-block start down, so a misaligned resume would re-send
    bytes the file already holds and splice a duplicate into the middle of the audio.
    """
    with open(path, "ab", buffering=0) as sink:
        def drain(chunks):
            sink.write(b"".join(chunks))

        return await _transfer(client, start, length, segment, stall_limit,
                               drain, on_progress or (lambda _: None))


def _scan(raw):
    """Frame splitter. Returns (frames, bytes skipped, bytes consumed).

    Frames are [length][payload], but two things stop a straight walk from working: each
    440-byte block boundary carries a stray length byte with no payload, and the block grid
    itself can be shifted if any write in the file's history was short. So validate every
    candidate against the known TOC byte and slide forward a byte at a time when it fails.

    The consumed count lets a caller feed the stream in slices: whatever is left over is a
    partial frame straddling the slice boundary and must be carried into the next one.
    """
    frames, skipped, i = [], 0, 0
    while i + 1 < len(raw):
        length = raw[i]
        if raw[i + 1] == OPUS_TOC and 2 <= length <= 60:
            if i + 1 + length > len(raw):
                break          # truncated by the slice boundary, not corrupt
            frames.append(raw[i + 1:i + 1 + length])
            i += 1 + length
        else:
            skipped += 1
            i += 1
    return frames, skipped, i


def parse_frames(raw):
    """Splits the whole byte stream into Opus frames. Returns (frames, bytes skipped)."""
    frames, skipped, _ = _scan(raw)
    return frames, skipped


class Decoder:
    """A reusable Opus decoder.

    Worth holding on to rather than creating per batch: CELT carries overlap between
    consecutive frames, so a decoder that restarts partway through a recording puts a small
    discontinuity at every restart.
    """

    def __init__(self):
        err = ctypes.c_int()
        self.state = _opus.opus_decoder_create(RATE, 1, ctypes.byref(err))
        self.buf = (ctypes.c_int16 * (FRAME_SAMPLES * 8))()

    def decode(self, frames):
        """Decodes frames to mono int16 PCM. Returns (samples, frames that failed)."""
        pcm, bad = [], 0
        for frame in frames:
            n = _opus.opus_decode(self.state, frame, len(frame), self.buf, FRAME_SAMPLES * 8, 0)
            if n > 0:
                pcm.append(np.frombuffer(bytes(self.buf)[: n * 2], dtype=np.int16))
            else:
                bad += 1
        return (np.concatenate(pcm) if pcm else np.array([], dtype=np.int16)), bad


def decode(frames):
    """Decodes frames to mono int16 PCM. Returns (samples, frames that failed)."""
    return Decoder().decode(frames)


def write_wav(path, samples):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(samples.tobytes())


class DecodeResult:
    """Totals from a streamed decode; same numbers save_and_report() prints."""

    def __init__(self, frames, bad, skipped, samples, square_sum, peak):
        self.frames, self.bad, self.skipped = frames, bad, skipped
        self.samples, self.peak = samples, peak
        self.seconds = samples / RATE
        self.rms = math.sqrt(square_sum / samples) / 32768.0 if samples else 0.0

    @property
    def clipped(self):
        return self.peak >= 32767

    def summary(self):
        return (f"frames {self.frames:,}  decode errors {self.bad}  resync skips {self.skipped:,}\n"
                f"  duration {format_duration(self.seconds)}   "
                f"RMS {20*math.log10(self.rms+1e-12):.1f} dBFS   "
                f"peak {20*math.log10(self.peak/32768.0+1e-12):.1f} dBFS")


def decode_file_to_wav(raw_path, wav_path, on_progress=None, slice_bytes=4 << 20):
    """Decode a raw capture straight into a WAV, holding only a slice at a time.

    A whole-card pull is hours of audio, and 16 kHz mono PCM runs to about 115 MB per hour --
    decoding it the way the single-shot tools do would need gigabytes of RAM for the sample
    array alone. So the file is walked in slices, with each one appended to the WAV as it is
    decoded and only the running totals kept.

    Frames straddle slice boundaries, so the trailing partial frame is carried into the next
    slice. Dropping it instead would silently lose a frame every few megabytes.
    """
    decoder = Decoder()
    frames = bad = skipped = samples = peak = 0
    square_sum = 0.0

    with wave.open(wav_path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        carry = b""
        with open(raw_path, "rb") as source:
            while True:
                chunk = source.read(slice_bytes)
                if not chunk:
                    break
                buf = carry + chunk
                batch, slice_skipped, used = _scan(buf)
                carry = buf[used:]

                pcm, slice_bad = decoder.decode(batch)
                if len(pcm):
                    w.writeframes(pcm.tobytes())
                    as_float = pcm.astype(np.float64)
                    square_sum += float(np.sum(as_float ** 2))
                    peak = max(peak, int(np.max(np.abs(pcm))))
                    samples += len(pcm)

                frames += len(batch)
                bad += slice_bad
                skipped += slice_skipped
                if on_progress:
                    on_progress(source.tell())

    return DecodeResult(frames, bad, skipped, samples, square_sum, peak)


# --- Timestamp index -------------------------------------------------------------------------
#
# Each segment has a `.idx` sidecar holding a mark every 30 s, which is the only way to turn a
# byte offset into a time: the encoder is VBR, so a quiet hour occupies a fraction of a loud one
# and no bitrate arithmetic can stand in for it.

INDEX_RECORD = 16
INDEX_INTERVAL_S = 30


def parse_index(raw):
    """Records as (offset, epoch, uptime, boot_id), oldest first."""
    usable = len(raw) - (len(raw) % INDEX_RECORD)
    return [struct.unpack("<IIII", raw[i:i + INDEX_RECORD])
            for i in range(0, usable, INDEX_RECORD)]


def timeline(records):
    """Recorded-audio seconds elapsed at each mark, counting from the first.

    Elapsed time comes from the uptime column and is only meaningful within one boot, since a
    reset restarts it at zero — but a reset costs no audio either, because the file is simply
    appended to again when the device comes back. So a boot boundary contributes nothing and
    the walk continues through it.
    """
    marks = [0.0]
    for i in range(1, len(records)):
        step = records[i][2] - records[i - 1][2] if records[i][3] == records[i - 1][3] else 0
        marks.append(marks[-1] + max(step, 0))
    return marks


def wall_clock_at(records, marks, position):
    """Epoch seconds at a mark, or None if no reachable mark carries a date.

    Marks store epoch 0 until an app sets the clock (see set_time.py), so the date is recovered
    by taking the most recent dated mark and stepping back along the recorded timeline. Time
    spent rebooting is not in that timeline, so a window reaching back through a reset reads
    later than it truly was, by roughly the downtime.
    """
    anchor = next((i for i in range(len(records) - 1, -1, -1) if records[i][1] != 0), None)
    if anchor is None:
        return None
    return records[anchor][1] + (marks[position] - marks[anchor])


async def fetch_index(client, segment):
    """The timestamp index for a segment, as parsed records."""
    raw, _ = await download(client, 0, READ_TO_EOF, segment=segment, want_index=True)
    return parse_index(raw)


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

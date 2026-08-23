"""Pull every recording off the device, and survive being interrupted.

    python3 pull_all.py ~/Desktop/omi-archive     # start, or resume, a full pull
    python3 pull_all.py ~/Desktop/omi-archive --decode-only    # re-decode what is already down

A full card is hours of transfer -- 123 MB at ~15 KB/s is over two of them -- so this is built
around the assumption that it will not run to completion in one go. The connection drops, the
laptop sleeps, you press Ctrl-C. Run it again with the same directory and it picks up from the
last block it wrote.

The pull is fixed at a cutoff taken when it starts: everything recorded up to that moment, and
nothing after. Without that a pull could never end, because the device keeps recording while you
are reading and the target would run away from you. Resuming keeps the original cutoff, so
"everything before I started" still means the same instant however many attempts it takes.

Output is one WAV per segment, named for when the audio was recorded. The raw `.opus` captures
are kept next to them: they are the thing that was expensive to fetch, and decoding again is
seconds of local work.
"""
import argparse
import asyncio
import json
import os
import sys
import time

from bleak import BleakClient

import omi_sd

STATE_NAME = "pull-state.json"
STATE_VERSION = 1

# A dropped link is ordinary here, so retries are generous. The count only limits attempts that
# achieve nothing at all; any attempt that moves bytes resets it.
MAX_FUTILE_ATTEMPTS = 5
RECONNECT_PAUSE_S = 5

# Abandon a transfer that stops writing. The read loop has its own stall detection, but it can
# only act while it is running: a hang inside the Bluetooth stack reports nothing and waits for
# ever, which cost a two-hour pull once. Bytes landing on disk is the one signal that cannot lie,
# and at ~16 KB/s a healthy transfer writes every second, so this only ever fires on a real hang.
IDLE_LIMIT_S = 90
WATCH_TICK_S = 5
CONNECT_TIMEOUT_S = 60
DISCONNECT_TIMEOUT_S = 15


def part_path(out_dir, seq):
    return os.path.join(out_dir, f"seg-{seq:04d}.opus")


def state_path(out_dir):
    return os.path.join(out_dir, STATE_NAME)


def load_state(out_dir):
    try:
        with open(state_path(out_dir)) as f:
            state = json.load(f)
    except FileNotFoundError:
        return None
    if state.get("version") != STATE_VERSION:
        raise SystemExit(f"  {state_path(out_dir)} is version {state.get('version')}, "
                         f"expected {STATE_VERSION}; move it aside to start fresh")
    return state


def save_state(out_dir, state):
    # Written via a temporary file: a half-written state file read on the next run would be a
    # confusing way to lose a two-hour transfer.
    tmp = state_path(out_dir) + ".tmp"
    with open(tmp, "w") as f:
        json.dump(state, f, indent=2)
    os.replace(tmp, state_path(out_dir))


def on_disk(out_dir, part):
    """Bytes already fetched for a part, never counting past what the part should hold.

    The part files are the record of progress, not the state file. Bytes reach the disk about
    once a second while the state file is written once per segment, so trusting a byte count in
    the state file would mean re-fetching whatever the two disagreed about.
    """
    try:
        size = os.path.getsize(part_path(out_dir, part["seq"]))
    except FileNotFoundError:
        return 0
    return min(size, part["expected"]) if part["expected"] else size


def file_size(path):
    try:
        return os.path.getsize(path)
    except FileNotFoundError:
        return 0


async def guard_progress(coro, path, idle_limit=IDLE_LIMIT_S):
    """Run a transfer, abandoning it if `path` stops growing. Returns its result, or None if hung.

    Watches the file rather than the transfer because the failure this exists for is the transfer
    becoming unable to report anything at all.
    """
    task = asyncio.ensure_future(coro)
    size, idle_since = file_size(path), time.monotonic()

    while not task.done():
        await asyncio.sleep(WATCH_TICK_S)
        grown = file_size(path)
        if grown != size:
            size, idle_since = grown, time.monotonic()
        elif time.monotonic() - idle_since >= idle_limit:
            task.cancel()
            try:
                await task
            except (Exception, asyncio.CancelledError):
                pass
            return None

    return await task


def planned_bytes(part, segment_bytes):
    """What a part is expected to hold. Only the cutoff segment's length is known exactly."""
    return part["expected"] if part["expected"] else segment_bytes


async def build_plan(client, out_dir):
    """Snapshot the ring and work out what "everything recorded so far" means."""
    info = await omi_sd.read_info(client)
    print(f"  {info}")
    if not info.io_healthy:
        print(f"  WARNING: card reports {info.open_failures} open and {info.write_failures} "
              f"write failures; recording may have gaps")

    parts = []
    for seq in range(info.oldest_seq, info.newest_seq + 1):
        # Only the segment being recorded into has a length that matters: it is where the cutoff
        # falls. The rest are read to end of file, which avoids assuming they were rotated at
        # exactly the configured size.
        expected = info.newest_bytes if seq == info.newest_seq else None
        parts.append({"seq": seq, "expected": expected, "complete": False, "epoch": None})

    print(f"  reading timestamp indexes for {len(parts)} segments")
    for part in parts:
        number = part["seq"] - info.oldest_seq + 1
        records = await omi_sd.fetch_index(client, number)
        if records:
            part["epoch"] = omi_sd.wall_clock_at(records, omi_sd.timeline(records), 0)

    return {
        "version": STATE_VERSION,
        "started": time.time(),
        "cutoff_seq": info.newest_seq,
        "cutoff_bytes": info.newest_bytes,
        "segment_bytes": info.segment_bytes,
        "parts": parts,
    }


async def fetch_part(client, info, state, out_dir, part, progress, done_elsewhere):
    """Fetch one segment, resuming from whatever its file already holds."""
    seq = part["seq"]
    number = seq - info.oldest_seq + 1
    if number < 1:
        # The ring rotated far enough during the pull to drop this segment. Nothing to fetch,
        # and pretending otherwise would read whatever segment now sits at that number.
        print(f"  segment seq {seq} has been evicted since the pull started; skipping")
        part["complete"] = True
        part["evicted"] = True
        return 0
    if number > info.count:
        print(f"  segment seq {seq} is not on the card (number {number} of {info.count})")
        return 0

    path = part_path(out_dir, seq)
    have = on_disk(out_dir, part)

    # Resume on a block boundary. The firmware serves 440-byte blocks and rounds a mid-block
    # start down, so resuming from an unaligned length would re-send bytes the file already has
    # and splice a duplicate into the audio. Dropping the partial block costs nothing: it is
    # re-fetched immediately.
    aligned = have - (have % omi_sd.SD_BLE_SIZE)
    if aligned != have:
        with open(path, "r+b") as f:
            f.truncate(aligned)
        have = aligned

    expected = part["expected"]
    if expected is not None and have >= expected:
        part["complete"] = True
        return 0

    remaining = (expected - have) if expected is not None else omi_sd.READ_TO_EOF
    target = omi_sd.format_size(expected) if expected else "to end of file"
    resumed = f", resuming at {omi_sd.format_size(have)}" if have else ""
    print(f"\n  segment {number} (seq {seq}) -> {os.path.basename(path)}, {target}{resumed}")

    base = done_elsewhere + have
    began = time.time()
    outcome = await guard_progress(
        omi_sd.download_to_file(client, have, remaining, path, number,
                                on_progress=lambda got: progress.update(base + got)),
        path)

    elapsed = time.time() - began
    if outcome is None:
        reason = "hung"
        print(f"\n  nothing written for {IDLE_LIMIT_S}s; abandoning this link")
    else:
        _, elapsed, reason = outcome

    # Measured from the file, not reported by the transfer: on a hang there is no report, and the
    # bytes on disk are what actually survived either way.
    size = os.path.getsize(path)
    written = size - have
    if expected is not None:
        if size >= expected:
            # The final notification can overshoot the cutoff; trim so the segment ends exactly
            # where the snapshot said it did.
            if size > expected:
                with open(path, "r+b") as f:
                    f.truncate(expected)
            part["complete"] = True
    elif reason == "eof":
        part["complete"] = True

    status = "complete" if part["complete"] else f"incomplete ({reason})"
    print(f"  segment {number}: +{omi_sd.format_size(written)} in "
          f"{omi_sd.format_duration(elapsed)}, {status}")
    save_state(out_dir, state)
    if reason == "hung":
        # The link is unusable and the client is mid-operation; get a fresh one rather than
        # starting the next segment down a connection that has already stopped answering.
        raise ConnectionError("transfer hung")
    return written


async def transfer(state, out_dir):
    """Connect and fetch whatever is still outstanding."""
    device = await omi_sd.find_device()
    if not device:
        print("  device not found - is it advertising? a failed SD mount stops BLE entirely")
        return

    client = BleakClient(device, timeout=30.0)
    await asyncio.wait_for(client.connect(), timeout=CONNECT_TIMEOUT_S)
    try:
        info = await omi_sd.read_info(client)

        total = sum(planned_bytes(p, state["segment_bytes"]) for p in state["parts"])
        already = sum(on_disk(out_dir, p) for p in state["parts"])
        progress = omi_sd.Progress("all audio", total, bar=True, resumed=already)

        for part in state["parts"]:
            if part["complete"]:
                continue
            done_elsewhere = sum(on_disk(out_dir, p) for p in state["parts"] if p is not part)
            await fetch_part(client, info, state, out_dir, part, progress, done_elsewhere)

        progress.finish(sum(on_disk(out_dir, p) for p in state["parts"]))
    finally:
        # Bounded for the same reason the transfer teardown is: disconnecting talks to a peer
        # that may already have stopped answering, and this runs on the path where it has.
        try:
            await asyncio.wait_for(client.disconnect(), timeout=DISCONNECT_TIMEOUT_S)
        except Exception:
            print("  disconnect timed out; abandoning the link")


def estimate_starts(state, decoded):
    """When each decoded segment began. Returns (start epochs, whether any was reconstructed).

    A segment's own index carries the date when an app has set the device clock. When it has
    not — the clock resets on every power cycle — the marks hold epoch 0 and there is nothing to
    read, so the times are reconstructed backwards from the cutoff: the newest segment's audio
    ends the instant the pull started, and each earlier segment ends where the next one begins.

    The reconstruction is only as good as that chain. Time spent powered off is not in the
    recording, so segments before a shutdown read later than they truly were, by the length of
    the gap. Run set_time.py and the dates become real rather than inferred.
    """
    starts = [None] * len(decoded)
    reconstructed = False
    ends_at = state["started"]
    for i in range(len(decoded) - 1, -1, -1):
        part, _, result = decoded[i]
        if part["epoch"]:
            starts[i] = part["epoch"]
        else:
            starts[i] = ends_at - result.seconds
            reconstructed = True
        ends_at = starts[i]
    return starts, reconstructed


def decode_all(state, out_dir):
    """Turn every fetched segment into a WAV named for when it was recorded."""
    print("\n  decoding")
    decoded = []
    for part in state["parts"]:
        raw = part_path(out_dir, part["seq"])
        if not os.path.exists(raw) or os.path.getsize(raw) == 0:
            continue

        # Decoded under a neutral name first: the recording's start time is only known once its
        # length is, and the length is what the decode measures.
        interim = os.path.join(out_dir, f"seg-{part['seq']:04d}.wav")
        size = os.path.getsize(raw)
        print(f"\n  {os.path.basename(raw)} ({omi_sd.format_size(size)})")
        progress = omi_sd.Progress("decoding", size, bar=True)
        result = omi_sd.decode_file_to_wav(raw, interim, on_progress=progress.update)
        progress.finish(size)

        print(f"  {result.summary()}")
        if result.bad:
            print(f"  NOTE: {result.bad} frames failed to decode")
        if result.clipped:
            print("  NOTE: clipping at full scale - lower MIC_GAIN in config.h")
        decoded.append((part, interim, result))

    if not decoded:
        print("  nothing to decode yet")
        return

    starts, reconstructed = estimate_starts(state, decoded)
    print()
    total_seconds = 0.0
    for (part, interim, result), start in zip(decoded, starts):
        stamp = time.strftime("%Y%m%d-%H%M%S", time.localtime(start))
        wav = os.path.join(out_dir, f"omi-{stamp}.wav")
        os.replace(interim, wav)
        # Stamp the file so it sorts and displays by when the audio happened, not when it synced.
        os.utime(wav, (time.time(), start + result.seconds))
        total_seconds += result.seconds
        print(f"  {os.path.basename(wav)}   "
              f"{time.strftime('%a %d %b %H:%M', time.localtime(start))} -> "
              f"{time.strftime('%H:%M', time.localtime(start + result.seconds))}   "
              f"{omi_sd.format_duration(result.seconds)}")

    if reconstructed:
        print("\n  times are reconstructed from recorded length, not read from the device: the\n"
              "  clock was never set, so anything before a power-off reads late by the gap.\n"
              "  Run set_time.py to make future recordings carry real dates.")
    print(f"\n  {omi_sd.format_duration(total_seconds)} of audio in {out_dir}")


async def run(out_dir, decode_only):
    began = time.time()
    os.makedirs(out_dir, exist_ok=True)
    state = load_state(out_dir)

    if decode_only:
        if not state:
            print(f"  nothing to decode: no {STATE_NAME} in {out_dir}")
            return 1
        decode_all(state, out_dir)
        return 0

    if state:
        outstanding = [p for p in state["parts"] if not p["complete"]]
        print(f"  resuming an earlier pull: {len(outstanding)} of {len(state['parts'])} "
              f"segments outstanding")
    else:
        device = await omi_sd.find_device()
        if not device:
            print("  device not found - is it advertising? a failed SD mount stops BLE entirely")
            return 1
        async with BleakClient(device, timeout=30.0) as client:
            state = await build_plan(client, out_dir)
        save_state(out_dir, state)

    total = sum(planned_bytes(p, state["segment_bytes"]) for p in state["parts"])
    cutoff = time.strftime("%H:%M:%S", time.localtime(state["started"]))
    print(f"  pulling {omi_sd.format_size(total)} recorded before {cutoff}, "
          f"about {omi_sd.format_duration(total / 15360)} at 15 KB/s")
    print(f"  interrupt whenever; rerunning with {out_dir} continues from the last block\n")

    futile = 0
    while any(not p["complete"] for p in state["parts"]):
        if futile >= MAX_FUTILE_ATTEMPTS:
            print(f"\n  giving up after {futile} attempts that moved nothing. "
                  f"Rerun to try again; nothing already fetched is lost.")
            return 1
        # Measured from the files rather than reported by the session, so bytes still count when
        # the session ends by throwing -- which is the normal way a dropped link ends.
        before = sum(on_disk(out_dir, p) for p in state["parts"])
        try:
            await transfer(state, out_dir)
        except KeyboardInterrupt:
            raise
        except Exception as exc:
            print(f"\n  connection lost: {type(exc).__name__}: {exc}")
        moved = sum(on_disk(out_dir, p) for p in state["parts"]) - before
        save_state(out_dir, state)

        if any(not p["complete"] for p in state["parts"]):
            futile = 0 if moved else futile + 1
            if futile:
                print(f"  attempt moved nothing ({futile} of {MAX_FUTILE_ATTEMPTS})")
            print(f"  reconnecting in {RECONNECT_PAUSE_S}s")
            await asyncio.sleep(RECONNECT_PAUSE_S)

    print(f"\n  all segments fetched in {omi_sd.format_duration(time.time() - began)}")
    decode_all(state, out_dir)
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out_dir", help="directory for the captures, the WAVs and the resume state")
    ap.add_argument("--decode-only", action="store_true",
                    help="re-decode what is already downloaded, without touching the device")
    args = ap.parse_args()
    try:
        sys.exit(asyncio.run(run(args.out_dir, args.decode_only)))
    except KeyboardInterrupt:
        print("\n  stopped; rerun with the same directory to continue")
        sys.exit(130)


if __name__ == "__main__":
    main()

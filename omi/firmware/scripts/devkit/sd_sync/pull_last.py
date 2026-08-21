"""Pull the last N minutes of audio off the recorder and name the file by when it was recorded.

Byte offsets cannot be derived from a bitrate: the encoder is VBR and drops to near nothing
during silence, so a quiet hour occupies a fraction of a loud one. This walks the per-segment
timestamp index instead, which records the byte offset reached every 30 s.

    python3 pull_last.py 30                     # -> ./omi-20260821-090632.wav
    python3 pull_last.py 30 ~/Desktop           # names the file in that directory
    python3 pull_last.py 30 ~/Desktop/take.wav  # exact path
"""
import argparse
import asyncio
import os
import struct
import sys
import time

from bleak import BleakClient

import omi_sd

INDEX_RECORD = 16
INDEX_INTERVAL_S = 30
READ_TO_EOF = omi_sd.READ_TO_EOF


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


def mark_before(records, marks, seconds_back):
    """Index of the newest mark at least `seconds_back` of audio before the end.

    Erring towards an older mark hands back slightly more than asked for, which is the safer
    direction when the point is "everything since".
    """
    target = marks[-1] - seconds_back
    for i in range(len(records) - 1, -1, -1):
        if marks[i] <= target:
            return i
    return 0


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


def resolve_out_path(out, start_epoch):
    """Turn a directory (or nothing) into a filename stamped with the recording's start."""
    stamp = time.strftime("%Y%m%d-%H%M%S", time.localtime(start_epoch))
    if out is None:
        return f"omi-{stamp}.wav"
    if os.path.isdir(out):
        return os.path.join(out, f"omi-{stamp}.wav")
    return out


async def fetch_index(client, segment):
    raw, _ = await omi_sd.download(client, 0, READ_TO_EOF, segment=segment, want_index=True)
    return parse_index(raw)


async def run(minutes, out):
    began = time.time()
    device = await omi_sd.find_device()
    if not device:
        print("device not found - is it advertising? a failed SD mount stops BLE entirely")
        return 1

    seconds = minutes * 60
    async with BleakClient(device, timeout=30.0) as client:
        info = await omi_sd.read_info(client)
        print(f"  {info}")

        records = await fetch_index(client, info.count)
        if not records:
            print("  no index on the segment being recorded; cannot place audio in time")
            return 1

        marks = timeline(records)
        position = mark_before(records, marks, seconds)
        start, covered = records[position][0], marks[-1] - marks[position]
        start_epoch = wall_clock_at(records, marks, position)
        spans_reboot = len({r[3] for r in records[position:]}) > 1
        print(f"  segment {info.count}: index has {len(records)} marks, "
              f"taking from offset {start:,} ({covered/60:.1f} min back)")

        parts, spent = [], 0.0
        # The window can reach past the start of this segment, in which case the rest lives in
        # the one before it. Frames are self-delimiting and the parser resynchronises, so the
        # two spans can simply be concatenated.
        if covered + INDEX_INTERVAL_S < seconds and info.count > 1:
            older = info.count - 1
            older_records = await fetch_index(client, older)
            if older_records:
                older_marks = timeline(older_records)
                older_position = mark_before(older_records, older_marks, seconds - covered)
                older_start = older_records[older_position][0]
                covered += older_marks[-1] - older_marks[older_position]
                older_epoch = wall_clock_at(older_records, older_marks, older_position)
                if older_epoch is not None:
                    start_epoch = older_epoch
                elif start_epoch is not None:
                    start_epoch -= older_marks[-1] - older_marks[older_position]
                spans_reboot = spans_reboot or len({r[3] for r in older_records[older_position:]}) > 1
                print(f"  window crosses a rotation; taking "
                      f"{(older_marks[-1] - older_marks[older_position])/60:.1f} min "
                      f"from segment {older} at offset {older_start:,}")
                raw, elapsed = await omi_sd.download(client, older_start, READ_TO_EOF,
                                                     segment=older)
                print(f"  segment {older}: {len(raw):,} bytes in {elapsed:.0f}s")
                parts.append(raw)
                spent += elapsed
            else:
                print(f"  segment {older} has no index; returning what this segment holds")

        if start_epoch is None:
            # No mark carries a date, so the best available anchor is the host: the pull ends
            # at roughly now, and the window reaches `covered` seconds back from there.
            start_epoch = time.time() - covered
            print("  clock was never set on the device; timestamp is the host's estimate "
                  "(run set_time.py to fix future recordings)")

        out_path = resolve_out_path(out, start_epoch)
        length = max(info.newest_bytes - start, 0)
        print(f"  pulling {length:,} bytes from segment {info.count}", flush=True)
        raw, elapsed = await omi_sd.download(client, start, length, segment=info.count)
        parts.append(raw)
        spent += elapsed

        duration = omi_sd.save_and_report(b"".join(parts), spent, out_path)
        if duration is None:
            return 1

        # The decoded length is exact, where `covered` only reaches the last index mark and so
        # misses up to one 30 s interval of audio at the end.
        started, ended = start_epoch, start_epoch + duration
        print(f"  recorded {time.strftime('%a %d %b %H:%M:%S', time.localtime(started))} "
              f"-> {time.strftime('%H:%M:%S', time.localtime(ended))}")
        if spans_reboot:
            print("  window spans a reboot; the start may read up to the downtime late, since "
                  "time spent off is not in the recording")
        # Stamp the file so it sorts and displays by when the audio happened, not when it synced.
        os.utime(out_path, (time.time(), ended))
        print(f"  total {omi_sd.format_duration(time.time() - began)} "
              f"for {omi_sd.format_duration(duration)} of audio")
        return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("minutes", type=float, help="how far back to reach")
    ap.add_argument("out", nargs="?",
                    help="output file, or a directory to name the file by timestamp "
                         "(default: current directory)")
    args = ap.parse_args()
    sys.exit(asyncio.run(run(args.minutes, args.out)))


if __name__ == "__main__":
    main()

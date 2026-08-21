/**
 * Extracts Opus frames from the raw byte stream pulled off the device's SD card.
 *
 * On-card framing is [length][payload], but a straight walk does not work. Two
 * things break it, both documented in omi/firmware/devkit/DEBUGGING.md:
 *
 *   1. When a frame does not fit in the current 440-byte block the firmware
 *      writes its length byte at the block boundary and flushes, so that length
 *      byte has no payload behind it.
 *   2. The firmware never clears the tail of its block buffer, so every block
 *      carries roughly 12 bytes of stale data from the previous block.
 *
 * A short write anywhere in the file's history also shifts the whole block grid.
 * So instead of trusting alignment we validate every candidate against the known
 * TOC byte and slide forward one byte at a time when it fails.
 */

import { OPUS_FRAMES_PER_SECOND, OPUS_TOC } from '../ble/constants';

/** Widest plausible frame at the firmware's fixed 20 kbps CELT settings. */
const MIN_FRAME_BYTES = 2;
const MAX_FRAME_BYTES = 60;

export interface ParsedFrames {
  /** Each frame includes its leading TOC byte, ready to hand to an Opus decoder. */
  frames: Uint8Array[];
  /**
   * Bytes discarded while resynchronising. A few percent of the input is normal
   * -- these are the stray boundary length bytes and the stale block tails.
   */
  skipped: number;
}

export function parseOpusFrames(raw: Uint8Array): ParsedFrames {
  const frames: Uint8Array[] = [];
  let skipped = 0;
  let i = 0;

  while (i + 1 < raw.length) {
    const length = raw[i];
    const fits = i + 1 + length <= raw.length;

    if (
      raw[i + 1] === OPUS_TOC &&
      length >= MIN_FRAME_BYTES &&
      length <= MAX_FRAME_BYTES &&
      fits
    ) {
      frames.push(raw.subarray(i + 1, i + 1 + length));
      i += 1 + length;
    } else {
      skipped += 1;
      i += 1;
    }
  }

  return { frames, skipped };
}

/** Frames are a fixed 10 ms each, so duration follows directly from the count. */
export function framesToSeconds(frameCount: number): number {
  return frameCount / OPUS_FRAMES_PER_SECOND;
}

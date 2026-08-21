/**
 * Decisions the sync loop makes about what to fetch and what is safe to store.
 *
 * Kept free of React Native imports so the rules can be tested directly; the
 * engine supplies the real filesystem lookup.
 */

import { SD_BLE_SIZE } from '../ble/constants';
import { alignToBlock, segmentTotalBytes, type RingInfo } from '../ble/protocol';

export interface PlanItem {
  seq: number;
  /** Block-aligned offset to resume from. */
  start: number;
  /** Segment length the device reports right now. */
  total: number;
  remaining: number;
}

/**
 * Works out what to pull, oldest first.
 *
 * The newest segment is skipped unless a whole block has accumulated. It is
 * still being written to, so a sub-block read would be truncated away again by
 * `writableLength` and we would have spent a round trip for nothing.
 */
export function buildPlan(info: RingInfo, onDiskFor: (seq: number) => number): PlanItem[] {
  const plan: PlanItem[] = [];

  for (let seq = info.oldestSeq; seq <= info.newestSeq; seq += 1) {
    const total = segmentTotalBytes(info, seq);
    if (total <= 0) {
      continue;
    }

    // Align even though the stored size should already be aligned: a mid-block
    // start makes every following frame unparseable, so it is worth being
    // defensive about a truncated file.
    const start = alignToBlock(Math.min(onDiskFor(seq), total));
    const remaining = total - start;
    const worthPulling = seq === info.newestSeq ? remaining >= SD_BLE_SIZE : remaining > 0;

    if (worthPulling) {
      plan.push({ seq, start, total, remaining });
    }
  }

  return plan;
}

/**
 * How much of a buffered run may be committed to disk.
 *
 * Only a sealed segment may end on a partial block. The newest segment keeps
 * growing, so storing its short tail would make the file size stop being a
 * multiple of the block size; the next resume would round back down to the
 * previous boundary and the device would re-send bytes we already hold,
 * duplicating audio and shifting the frame grid. Dropping the tail costs at most
 * one block, which arrives whole on the next pass.
 */
export function writableLength(bufferedBytes: number, sealed: boolean, final: boolean): number {
  if (final && sealed) {
    return bufferedBytes;
  }
  return bufferedBytes - (bufferedBytes % SD_BLE_SIZE);
}

/**
 * On-disk layout for pulled audio.
 *
 *   recordings/segment-<seq>.bin   raw bytes exactly as the device served them
 *   recordings/index-<seq>.idx     the segment's timestamp sidecar, when we got one
 *   recordings/manifest.json       sync progress (see ../sync/progressStore)
 *
 * Storing the device's byte stream verbatim is what makes resume trivial: the
 * next read offset is simply the file size, so a crash costs at most one block.
 * Frames are parsed lazily at playback time rather than at sync time.
 */

import { Directory, File, Paths } from 'expo-file-system';

import { RECORDING_BYTES_PER_SECOND, SD_BLE_SIZE } from '../ble/constants';
import type { IndexRecord } from '../ble/protocol';

export const RECORDINGS_DIR_NAME = 'recordings';

/**
 * Playable chunk size: 325 blocks, which is about 60 seconds of audio. Kept an
 * exact multiple of the block size so a chunk boundary is always a block
 * boundary, and bounded so decoding one never has to hold a whole multi-hundred
 * megabyte segment in memory.
 */
export const CHUNK_BYTES = SD_BLE_SIZE * 325;

export function recordingsDirectory(): Directory {
  const dir = new Directory(Paths.document, RECORDINGS_DIR_NAME);
  if (!dir.exists) {
    dir.create({ intermediates: true, idempotent: true });
  }
  return dir;
}

export function segmentFile(seq: number): File {
  return new File(recordingsDirectory(), `segment-${seq}.bin`);
}

export function indexFile(seq: number): File {
  return new File(recordingsDirectory(), `index-${seq}.idx`);
}

/** Bytes already pulled for a segment. This is the resume offset. */
export function bytesOnDisk(seq: number): number {
  const file = segmentFile(seq);
  return file.exists ? file.size : 0;
}

/**
 * Appends device bytes to a segment. Returns the new size.
 *
 * Opening and closing per flush rather than holding a handle open for the whole
 * transfer keeps the file consistent if the app is killed mid-sync.
 */
export function appendToSegment(seq: number, bytes: Uint8Array): number {
  const file = segmentFile(seq);
  if (!file.exists) {
    file.create({ intermediates: true, overwrite: false });
  }
  const handle = file.open();
  try {
    handle.offset = handle.size ?? 0;
    handle.writeBytes(bytes);
  } finally {
    handle.close();
  }
  return file.size;
}

/** Reads a byte range out of a segment, clamped to what is actually on disk. */
export function readSegmentRange(seq: number, start: number, length: number): Uint8Array {
  const file = segmentFile(seq);
  if (!file.exists) {
    return new Uint8Array(0);
  }
  const available = Math.max(0, Math.min(length, file.size - start));
  if (available <= 0) {
    return new Uint8Array(0);
  }
  const handle = file.open();
  try {
    handle.offset = start;
    return new Uint8Array(handle.readBytes(available));
  } finally {
    handle.close();
  }
}

export function writeIndexFile(seq: number, bytes: Uint8Array): void {
  const file = indexFile(seq);
  if (!file.exists) {
    file.create({ intermediates: true, overwrite: true });
  }
  file.write(bytes);
}

export function deleteSegmentFiles(seq: number): void {
  for (const file of [segmentFile(seq), indexFile(seq)]) {
    if (file.exists) {
      file.delete();
    }
  }
}

export interface RecordingChunk {
  id: string;
  seq: number;
  index: number;
  startByte: number;
  byteLength: number;
  /** Wall-clock start, when the device's timestamp index told us one. */
  startEpoch: number | null;
  /** Estimated from the byte count; the player reports the exact figure once decoded. */
  approxSeconds: number;
}

/**
 * Interpolates a wall-clock time for a byte offset using the nearest preceding
 * index record. Records are written every 30 s, and the recording bitrate is
 * near-constant, so interpolating inside a record is accurate to well under a
 * second.
 */
function epochForOffset(offset: number, records: IndexRecord[]): number | null {
  let best: IndexRecord | null = null;
  for (const record of records) {
    if (record.epoch > 0 && record.offset <= offset && (!best || record.offset > best.offset)) {
      best = record;
    }
  }
  if (!best) {
    return null;
  }
  return best.epoch + (offset - best.offset) / RECORDING_BYTES_PER_SECOND;
}

/** Splits a segment's bytes into bounded, playable chunks. */
export function chunksForSegment(
  seq: number,
  totalBytes: number,
  indexRecords: IndexRecord[] = [],
): RecordingChunk[] {
  const chunks: RecordingChunk[] = [];
  for (let start = 0, index = 0; start < totalBytes; start += CHUNK_BYTES, index += 1) {
    const byteLength = Math.min(CHUNK_BYTES, totalBytes - start);
    chunks.push({
      id: `${seq}:${index}`,
      seq,
      index,
      startByte: start,
      byteLength,
      startEpoch: epochForOffset(start, indexRecords),
      approxSeconds: byteLength / RECORDING_BYTES_PER_SECOND,
    });
  }
  return chunks;
}

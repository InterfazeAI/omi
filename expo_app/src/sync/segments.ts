/**
 * The sync manifest's shape and its pure transformations.
 *
 * Everything is keyed by the device's sequence number, never by segment number.
 * Segment numbers are positions in a ring that shift down whenever a segment is
 * evicted, so a stored segment number silently starts pointing at different
 * audio; a sequence number does not.
 *
 * Reading and writing the file lives in ./progressStore; this module stays free
 * of native imports so the sync engine can be tested under `node --test`.
 */

import type { IndexRecord } from '../ble/protocol';

export const MANIFEST_VERSION = 1;

export interface SegmentRecord {
  seq: number;
  deviceId: string;
  /** Bytes on disk. Mirrors the .bin size; the file is the source of truth. */
  bytesPulled: number;
  /** Segment length the device last reported. */
  deviceBytes: number;
  /** True once we pulled through the device's end of that segment. */
  complete: boolean;
  /** The device has rotated past this segment, so no more of it can ever arrive. */
  evicted: boolean;
  indexRecords: IndexRecord[];
  updatedAt: number;
}

export interface Manifest {
  version: number;
  segments: SegmentRecord[];
}

export function upsertSegment(
  segments: SegmentRecord[],
  update: Partial<SegmentRecord> & { seq: number },
): SegmentRecord[] {
  const existing = segments.find((segment) => segment.seq === update.seq);
  const merged: SegmentRecord = {
    seq: update.seq,
    deviceId: update.deviceId ?? existing?.deviceId ?? '',
    bytesPulled: update.bytesPulled ?? existing?.bytesPulled ?? 0,
    deviceBytes: update.deviceBytes ?? existing?.deviceBytes ?? 0,
    complete: update.complete ?? existing?.complete ?? false,
    evicted: update.evicted ?? existing?.evicted ?? false,
    indexRecords: update.indexRecords ?? existing?.indexRecords ?? [],
    updatedAt: Date.now(),
  };

  const next = segments.filter((segment) => segment.seq !== update.seq);
  next.push(merged);
  return next.sort((a, b) => a.seq - b.seq);
}

/**
 * Decodes manifest JSON, taking each segment's byte count from the filesystem
 * rather than the file: a crash between the last append and the last save would
 * otherwise under-report progress. Unparseable JSON yields an empty list, since
 * the .bin files are the real data and the next sync rebuilds the manifest.
 */
export function parseManifest(text: string, bytesOnDisk: (seq: number) => number): SegmentRecord[] {
  let manifest: Manifest;
  try {
    manifest = JSON.parse(text) as Manifest;
  } catch {
    return [];
  }

  if (!Array.isArray(manifest?.segments)) {
    return [];
  }

  return manifest.segments
    .filter((segment) => typeof segment?.seq === 'number')
    .map((segment) => ({
      ...segment,
      indexRecords: Array.isArray(segment.indexRecords) ? segment.indexRecords : [],
      bytesPulled: bytesOnDisk(segment.seq),
    }))
    .sort((a, b) => a.seq - b.seq);
}

export function serializeManifest(segments: SegmentRecord[]): string {
  const manifest: Manifest = {
    version: MANIFEST_VERSION,
    segments: [...segments].sort((a, b) => a.seq - b.seq),
  };
  return JSON.stringify(manifest);
}

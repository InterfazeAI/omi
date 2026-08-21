/**
 * Pure encode/decode for the storage protocol. No React Native imports here, so
 * this module is directly unit-testable under `node --test`.
 */

import {
  INDEX_RECORD_SIZE,
  SD_BLE_SIZE,
  SEGMENT_INDEX_FLAG,
  STORAGE_INFO_BYTES,
  describeStatus,
} from './constants';

/**
 * Everything the device sends arrives on one characteristic and is told apart
 * purely by length: a single byte is a status code, anything longer is file
 * data. There is no header, sequence number, or checksum.
 */
export type StorageNotification =
  | { kind: 'status'; status: number }
  | { kind: 'data'; bytes: Uint8Array };

export class StorageCommandError extends Error {
  readonly status: number;

  constructor(status: number) {
    super(describeStatus(status));
    this.name = 'StorageCommandError';
    this.status = status;
  }
}

export interface RingInfo {
  /** Bytes written so far to the segment currently being recorded. Exact. */
  newestBytes: number;
  /** The device's own persisted read cursor. A hint only -- it carries no segment number. */
  savedOffset: number;
  /** Segments on the card. Also the segment number of the newest one. */
  count: number;
  /** Sequence number of the oldest segment still on the card. */
  oldestSeq: number;
  /** Sequence number of the segment currently being recorded. */
  newestSeq: number;
  /** Segment size target. Sealed segments are this big to within one 8 KB write batch. */
  segmentBytes: number;
}

function readU32LE(bytes: Uint8Array, at: number): number {
  return (
    (bytes[at] | (bytes[at + 1] << 8) | (bytes[at + 2] << 16) | (bytes[at + 3] << 24)) >>> 0
  );
}

/**
 * Decodes the 21-byte storage info characteristic. All fields are little-endian,
 * in contrast to the big-endian offset in a command frame.
 *
 * Firmware older than the ring rewrite returns only 8 bytes: a single file that
 * grows until it stops, which we present as a one-segment ring.
 */
export function parseRingInfo(bytes: Uint8Array): RingInfo {
  if (bytes.length < 8) {
    throw new Error(`storage info too short: ${bytes.length} bytes`);
  }

  const newestBytes = readU32LE(bytes, 0);
  const savedOffset = readU32LE(bytes, 4);

  if (bytes.length < STORAGE_INFO_BYTES) {
    return {
      newestBytes,
      savedOffset,
      count: 1,
      oldestSeq: 1,
      newestSeq: 1,
      segmentBytes: 0,
    };
  }

  return {
    newestBytes,
    savedOffset,
    count: bytes[8],
    oldestSeq: readU32LE(bytes, 9),
    newestSeq: readU32LE(bytes, 13),
    segmentBytes: readU32LE(bytes, 17),
  };
}

/** Approximate total across the ring. Only the newest segment's length is exact. */
export function totalRingBytes(info: RingInfo): number {
  return Math.max(0, info.count - 1) * info.segmentBytes + info.newestBytes;
}

/**
 * Segment numbers are positions in the ring, oldest first, so an eviction shifts
 * them all down. Sequence numbers are stable, so we always address a segment by
 * converting its sequence number at the moment we talk to the device.
 *
 * Returns null when that sequence number is no longer on the card.
 */
export function segmentNumberForSeq(info: RingInfo, seq: number): number | null {
  if (seq < info.oldestSeq || seq > info.newestSeq) {
    return null;
  }
  const num = info.count - (info.newestSeq - seq);
  return num >= 1 && num <= info.count ? num : null;
}

/** How many bytes that segment holds right now. */
export function segmentTotalBytes(info: RingInfo, seq: number): number {
  return seq === info.newestSeq ? info.newestBytes : info.segmentBytes;
}

/**
 * Builds the 6-byte command frame. The offset is big-endian; the info
 * characteristic is little-endian. Mixing them up is the classic bug here.
 */
export function encodeCommand(command: number, segment: number, offset = 0): Uint8Array {
  return new Uint8Array([
    command & 0xff,
    segment & 0xff,
    (offset >>> 24) & 0xff,
    (offset >>> 16) & 0xff,
    (offset >>> 8) & 0xff,
    offset & 0xff,
  ]);
}

/** Addresses a segment's timestamp index rather than its audio. Reads only. */
export function indexSegment(segment: number): number {
  return (segment | SEGMENT_INDEX_FLAG) & 0xff;
}

/**
 * The firmware rounds a read offset down to a block boundary, and a mid-block
 * start makes every following frame unparseable. Round the same way client-side
 * so our byte accounting matches the device's.
 */
export function alignToBlock(offset: number): number {
  return offset - (offset % SD_BLE_SIZE);
}

export interface IndexRecord {
  /** Byte offset within the segment that this timestamp applies to. */
  offset: number;
  /** UTC epoch seconds, or 0 if the device clock was never synced. */
  epoch: number;
  uptimeSeconds: number;
  bootId: number;
}

/** Decodes the .idx sidecar: 16-byte records, all little-endian, appended every 30 s. */
export function parseIndexRecords(bytes: Uint8Array): IndexRecord[] {
  const records: IndexRecord[] = [];
  for (let i = 0; i + INDEX_RECORD_SIZE <= bytes.length; i += INDEX_RECORD_SIZE) {
    records.push({
      offset: readU32LE(bytes, i),
      epoch: readU32LE(bytes, i + 4),
      uptimeSeconds: readU32LE(bytes, i + 8),
      bootId: readU32LE(bytes, i + 12),
    });
  }
  return records;
}

/** Encodes UTC epoch seconds for the time-sync characteristic: 4 bytes little-endian. */
export function encodeEpochSeconds(epochSeconds: number): Uint8Array {
  const value = Math.floor(epochSeconds) >>> 0;
  return new Uint8Array([
    value & 0xff,
    (value >>> 8) & 0xff,
    (value >>> 16) & 0xff,
    (value >>> 24) & 0xff,
  ]);
}

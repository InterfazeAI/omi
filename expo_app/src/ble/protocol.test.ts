import assert from 'node:assert/strict';
import test from 'node:test';

import { SD_BLE_SIZE, StorageCommand } from './constants';
import {
  alignToBlock,
  encodeCommand,
  encodeEpochSeconds,
  indexSegment,
  parseIndexRecords,
  parseRingInfo,
  segmentNumberForSeq,
  segmentTotalBytes,
  totalRingBytes,
} from './protocol';

function infoBytes(fields: {
  newestBytes: number;
  savedOffset: number;
  count: number;
  oldestSeq: number;
  newestSeq: number;
  segmentBytes: number;
}): Uint8Array {
  const bytes = new Uint8Array(21);
  const view = new DataView(bytes.buffer);
  view.setUint32(0, fields.newestBytes, true);
  view.setUint32(4, fields.savedOffset, true);
  bytes[8] = fields.count;
  view.setUint32(9, fields.oldestSeq, true);
  view.setUint32(13, fields.newestSeq, true);
  view.setUint32(17, fields.segmentBytes, true);
  return bytes;
}

const SAMPLE = {
  newestBytes: 143_000,
  savedOffset: 88_000,
  count: 4,
  oldestSeq: 7,
  newestSeq: 10,
  segmentBytes: 450_560,
};

test('decodes the 21-byte info characteristic as little-endian', () => {
  const info = parseRingInfo(infoBytes(SAMPLE));
  assert.deepEqual(info, SAMPLE);
});

test('treats an 8-byte reply from pre-ring firmware as a single segment', () => {
  const bytes = new Uint8Array(8);
  new DataView(bytes.buffer).setUint32(0, 1234, true);
  new DataView(bytes.buffer).setUint32(4, 440, true);

  const info = parseRingInfo(bytes);
  assert.equal(info.newestBytes, 1234);
  assert.equal(info.savedOffset, 440);
  assert.equal(info.count, 1);
  assert.equal(info.oldestSeq, 1);
  assert.equal(info.newestSeq, 1);
  assert.equal(info.segmentBytes, 0);
});

test('rejects a reply that is too short to be meaningful', () => {
  assert.throws(() => parseRingInfo(new Uint8Array(4)), /too short/);
});

test('handles sizes above 2^31 without sign flipping', () => {
  const info = parseRingInfo(infoBytes({ ...SAMPLE, segmentBytes: 3_000_000_000 }));
  assert.equal(info.segmentBytes, 3_000_000_000);
});

test('maps stable sequence numbers to current ring positions', () => {
  const info = parseRingInfo(infoBytes(SAMPLE));
  assert.equal(segmentNumberForSeq(info, 10), 4, 'newest seq is the newest position');
  assert.equal(segmentNumberForSeq(info, 7), 1, 'oldest seq is position 1');
  assert.equal(segmentNumberForSeq(info, 8), 2);
});

test('reports an evicted or not-yet-recorded sequence as gone', () => {
  const info = parseRingInfo(infoBytes(SAMPLE));
  assert.equal(segmentNumberForSeq(info, 6), null, 'evicted');
  assert.equal(segmentNumberForSeq(info, 11), null, 'does not exist yet');
});

test('only the newest segment has an exact length', () => {
  const info = parseRingInfo(infoBytes(SAMPLE));
  assert.equal(segmentTotalBytes(info, 10), SAMPLE.newestBytes);
  assert.equal(segmentTotalBytes(info, 8), SAMPLE.segmentBytes);
  assert.equal(totalRingBytes(info), 3 * SAMPLE.segmentBytes + SAMPLE.newestBytes);
});

test('encodes the command frame with a big-endian offset', () => {
  assert.deepEqual(
    Array.from(encodeCommand(StorageCommand.Read, 3, 0x01020304)),
    [0x00, 0x03, 0x01, 0x02, 0x03, 0x04],
  );
  assert.deepEqual(
    Array.from(encodeCommand(StorageCommand.Stop, 1)),
    [0x03, 0x01, 0x00, 0x00, 0x00, 0x00],
  );
});

test('encodes offsets that exceed 2^31 without sign flipping', () => {
  assert.deepEqual(Array.from(encodeCommand(StorageCommand.Read, 1, 0xf0000001)), [
    0x00, 0x01, 0xf0, 0x00, 0x00, 0x01,
  ]);
});

test('sets the high bit to address a segment index', () => {
  assert.equal(indexSegment(3), 0x83);
});

test('rounds offsets down to a block boundary', () => {
  assert.equal(alignToBlock(0), 0);
  assert.equal(alignToBlock(SD_BLE_SIZE), SD_BLE_SIZE);
  assert.equal(alignToBlock(SD_BLE_SIZE + 1), SD_BLE_SIZE);
  assert.equal(alignToBlock(SD_BLE_SIZE * 3 - 1), SD_BLE_SIZE * 2);
});

test('decodes 16-byte index records and drops a partial tail', () => {
  const bytes = new Uint8Array(16 * 2 + 5);
  const view = new DataView(bytes.buffer);
  view.setUint32(0, 0, true);
  view.setUint32(4, 1_700_000_000, true);
  view.setUint32(8, 30, true);
  view.setUint32(12, 42, true);
  view.setUint32(16, 71_730, true);
  view.setUint32(20, 1_700_000_030, true);
  view.setUint32(24, 60, true);
  view.setUint32(28, 42, true);

  const records = parseIndexRecords(bytes);
  assert.equal(records.length, 2);
  assert.deepEqual(records[0], { offset: 0, epoch: 1_700_000_000, uptimeSeconds: 30, bootId: 42 });
  assert.equal(records[1].offset, 71_730);
});

test('encodes epoch seconds little-endian for the time-sync characteristic', () => {
  assert.deepEqual(Array.from(encodeEpochSeconds(0x01020304)), [0x04, 0x03, 0x02, 0x01]);
});

import assert from 'node:assert/strict';
import test from 'node:test';

import { SD_BLE_SIZE } from '../ble/constants';
import type { RingInfo } from '../ble/protocol';
import { buildPlan, writableLength } from './plan';

const SEGMENT_BYTES = 450_560; // lcm(8192, 440), the firmware's segment alignment

function info(overrides: Partial<RingInfo> = {}): RingInfo {
  return {
    newestBytes: 100_000,
    savedOffset: 0,
    count: 3,
    oldestSeq: 5,
    newestSeq: 7,
    segmentBytes: SEGMENT_BYTES,
    ...overrides,
  };
}

test('plans every segment from scratch when nothing is on disk', () => {
  const plan = buildPlan(info(), () => 0);

  assert.deepEqual(
    plan.map((item) => item.seq),
    [5, 6, 7],
  );
  assert.deepEqual(plan[0], { seq: 5, start: 0, total: SEGMENT_BYTES, remaining: SEGMENT_BYTES });
  assert.deepEqual(plan[2], { seq: 7, start: 0, total: 100_000, remaining: 100_000 });
});

test('resumes from what is already on disk', () => {
  const onDisk = new Map([
    [5, SEGMENT_BYTES],
    [6, 88_000],
    [7, 44_000],
  ]);
  const plan = buildPlan(info(), (seq) => onDisk.get(seq) ?? 0);

  assert.deepEqual(
    plan.map((item) => item.seq),
    [6, 7],
    'a fully pulled segment is not requested again',
  );
  assert.equal(plan[0].start, 88_000);
  assert.equal(plan[0].remaining, SEGMENT_BYTES - 88_000);
  assert.equal(plan[1].start, 44_000);
});

test('rounds a misaligned on-disk size down to a block boundary', () => {
  const plan = buildPlan(info({ oldestSeq: 6, newestSeq: 6, count: 1, newestBytes: 100_000 }), () => 44_001);
  assert.equal(plan[0].start, 44_000, '44001 rounds down to the previous 440 boundary');
});

test('skips the newest segment until a whole block has accumulated', () => {
  const base = info({ oldestSeq: 7, newestSeq: 7, count: 1, newestBytes: 44_000 + 100 });

  assert.equal(
    buildPlan(base, () => 44_000).length,
    0,
    'fewer than one block of new audio is not worth a round trip',
  );
  assert.equal(
    buildPlan(info({ oldestSeq: 7, newestSeq: 7, count: 1, newestBytes: 44_000 + SD_BLE_SIZE }), () => 44_000)
      .length,
    1,
    'a full block is',
  );
});

test('still finishes the last partial block of a sealed segment', () => {
  // A sealed segment never grows again, so its short tail is safe to take.
  const plan = buildPlan(
    info({ oldestSeq: 6, newestSeq: 7, count: 2, segmentBytes: 44_100 }),
    (seq) => (seq === 6 ? 44_000 : 0),
  );
  const sealed = plan.find((item) => item.seq === 6);
  assert.ok(sealed, 'sealed segment with a 100-byte tail is still planned');
  assert.equal(sealed.remaining, 100);
});

test('reports nothing to do when everything is already downloaded', () => {
  const onDisk = new Map([
    [5, SEGMENT_BYTES],
    [6, SEGMENT_BYTES],
    [7, 100_000],
  ]);
  assert.equal(buildPlan(info(), (seq) => onDisk.get(seq) ?? 0).length, 0);
});

test('ignores an empty ring', () => {
  assert.equal(
    buildPlan(info({ count: 1, oldestSeq: 1, newestSeq: 1, newestBytes: 0 }), () => 0).length,
    0,
  );
});

test('commits only whole blocks while a transfer is still running', () => {
  assert.equal(writableLength(SD_BLE_SIZE * 3 + 200, false, false), SD_BLE_SIZE * 3);
  assert.equal(writableLength(SD_BLE_SIZE * 3 + 200, true, false), SD_BLE_SIZE * 3);
  assert.equal(writableLength(200, false, false), 0);
});

test('drops the short tail of the growing newest segment', () => {
  // Storing it would leave the file size non-aligned, so the next resume would
  // round back down and the device would re-send bytes we already have.
  assert.equal(writableLength(SD_BLE_SIZE * 2 + 137, false, true), SD_BLE_SIZE * 2);
});

test('keeps the short tail of a sealed segment', () => {
  assert.equal(writableLength(SD_BLE_SIZE * 2 + 137, true, true), SD_BLE_SIZE * 2 + 137);
});

test('resume offset stays aligned across repeated partial syncs', () => {
  // Simulates three passes against a growing newest segment: each pass must
  // leave an aligned file and must never re-request bytes already stored.
  let onDisk = 0;
  const requestedStarts: number[] = [];

  for (const deviceBytes of [SD_BLE_SIZE * 4 + 61, SD_BLE_SIZE * 9 + 12, SD_BLE_SIZE * 15 + 300]) {
    const plan = buildPlan(
      info({ count: 1, oldestSeq: 1, newestSeq: 1, newestBytes: deviceBytes }),
      () => onDisk,
    );
    assert.equal(plan.length, 1);

    const item = plan[0];
    requestedStarts.push(item.start);
    assert.equal(item.start, onDisk, 'never re-reads bytes already on disk');

    onDisk += writableLength(item.remaining, false, true);
    assert.equal(onDisk % SD_BLE_SIZE, 0, 'file size stays block-aligned');
  }

  assert.deepEqual(requestedStarts, [0, SD_BLE_SIZE * 4, SD_BLE_SIZE * 9]);
});

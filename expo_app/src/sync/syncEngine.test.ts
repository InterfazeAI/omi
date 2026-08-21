/**
 * End-to-end tests for the sync loop against a fake DevKit.
 *
 * The fake speaks the real wire behaviour that has bitten us: blocks arrive as
 * notifications only after a READ write, a STOP does not stop blocks already in
 * flight, and the newest segment keeps growing between passes. Everything below
 * exercises the production `SyncEngine`; only the BLE transport and the
 * filesystem are substituted.
 */

import assert from 'node:assert/strict';
import test from 'node:test';

import { SD_BLE_SIZE, SEGMENT_INDEX_FLAG, StorageCommand, StorageStatus } from '../ble/constants';
import type { RingInfo, StorageNotification } from '../ble/protocol';
import { upsertSegment, type SegmentRecord } from './segments';
import { SyncEngine, type SyncClient, type SyncProgress, type SyncStore } from './syncEngine';

const BLOCK_INTERVAL_MS = 1;

const delay = (ms: number) => new Promise<void>((resolve) => setTimeout(resolve, ms));

/** Recognisable content so a duplicated or dropped block shows up as a mismatch. */
function segmentContent(seq: number, blocks: number, partialTail = 0): Uint8Array {
  const bytes = new Uint8Array(blocks * SD_BLE_SIZE + partialTail);
  for (let i = 0; i < bytes.length; i += 1) {
    bytes[i] = (seq * 31 + i * 7) & 0xff;
  }
  return bytes;
}

interface FakeDeviceOptions {
  /** Sequence number -> the bytes that segment holds. */
  content: Map<number, Uint8Array>;
  segmentBytes: number;
  /** Timestamp sidecars, keyed the same way. */
  index?: Map<number, Uint8Array>;
  /** Status to answer an index read with, instead of serving one. */
  indexStatus?: number;
  /** Blocks the device keeps sending after a STOP, as real firmware does. */
  stragglerBlocks?: number;
}

class FakeDevice implements SyncClient {
  readonly id = 'fake-devkit';

  readonly reads: { segment: number; offset: number }[] = [];

  readonly stops: number[] = [];

  private notify: ((notification: StorageNotification) => void) | null = null;

  private streaming = false;

  private options: FakeDeviceOptions;

  constructor(options: FakeDeviceOptions) {
    this.options = options;
  }

  update(options: Partial<FakeDeviceOptions>): void {
    this.options = { ...this.options, ...options };
  }

  private get seqs(): number[] {
    return [...this.options.content.keys()].sort((a, b) => a - b);
  }

  private get info(): RingInfo {
    const seqs = this.seqs;
    const newestSeq = seqs[seqs.length - 1];
    return {
      newestBytes: this.options.content.get(newestSeq)?.length ?? 0,
      savedOffset: 0,
      count: seqs.length,
      oldestSeq: seqs[0],
      newestSeq,
      segmentBytes: this.options.segmentBytes,
    };
  }

  /** Segment numbers are ring positions, oldest first. */
  private seqForSegment(segment: number): number | null {
    const seqs = this.seqs;
    return seqs[segment - 1] ?? null;
  }

  async readInfo(): Promise<RingInfo> {
    return this.info;
  }

  subscribe(onNotification: (notification: StorageNotification) => void): void {
    this.notify = onNotification;
  }

  unsubscribe(): void {
    this.notify = null;
  }

  async sendCommand(command: number, segment: number, offset = 0): Promise<void> {
    if (command === StorageCommand.Stop) {
      this.stops.push(segment);
      this.streaming = false;
      void this.sendStragglers();
      return;
    }
    if (command !== StorageCommand.Read) {
      return;
    }

    const isIndex = (segment & SEGMENT_INDEX_FLAG) !== 0;
    const seq = this.seqForSegment(segment & ~SEGMENT_INDEX_FLAG);
    this.reads.push({ segment, offset });

    if (seq === null) {
      this.emitStatus(StorageStatus.InvalidSegment);
      return;
    }
    if (isIndex && this.options.indexStatus !== undefined) {
      this.emitStatus(this.options.indexStatus);
      return;
    }

    const source = isIndex
      ? this.options.index?.get(seq) ?? new Uint8Array(0)
      : this.options.content.get(seq) ?? new Uint8Array(0);

    if (source.length === 0) {
      this.emitStatus(StorageStatus.EmptySegment);
      return;
    }
    if (offset >= source.length) {
      this.emitStatus(StorageStatus.OffsetPastEnd);
      return;
    }

    this.emitStatus(StorageStatus.Ok);
    void this.stream(source, offset);
  }

  async stopTransfer(segment: number): Promise<void> {
    await this.sendCommand(StorageCommand.Stop, Math.max(1, segment));
  }

  private emitStatus(status: number): void {
    this.notify?.({ kind: 'status', status });
  }

  private async stream(source: Uint8Array, offset: number): Promise<void> {
    this.streaming = true;
    for (let at = offset; at < source.length; at += SD_BLE_SIZE) {
      await delay(BLOCK_INTERVAL_MS);
      if (!this.streaming) {
        return;
      }
      this.notify?.({ kind: 'data', bytes: source.subarray(at, Math.min(at + SD_BLE_SIZE, source.length)) });
    }
    this.streaming = false;
    this.emitStatus(StorageStatus.EndOfTransfer);
  }

  /** A STOP only stops the queue being refilled; queued blocks still land. */
  private async sendStragglers(): Promise<void> {
    const count = this.options.stragglerBlocks ?? 0;
    for (let i = 0; i < count; i += 1) {
      await delay(BLOCK_INTERVAL_MS);
      this.notify?.({ kind: 'data', bytes: new Uint8Array(SD_BLE_SIZE).fill(0xee) });
    }
  }
}

class MemoryStore implements SyncStore {
  readonly files = new Map<number, Uint8Array>();

  readonly indexFiles = new Map<number, Uint8Array>();

  segments: SegmentRecord[] = [];

  saves = 0;

  bytesOnDisk(seq: number): number {
    return this.files.get(seq)?.length ?? 0;
  }

  appendToSegment(seq: number, bytes: Uint8Array): void {
    const existing = this.files.get(seq) ?? new Uint8Array(0);
    const merged = new Uint8Array(existing.length + bytes.length);
    merged.set(existing, 0);
    merged.set(bytes, existing.length);
    this.files.set(seq, merged);
  }

  writeIndexFile(seq: number, bytes: Uint8Array): void {
    this.indexFiles.set(seq, bytes);
  }

  loadSegments(): SegmentRecord[] {
    // Same contract as the real store: the filesystem, not the manifest, is the
    // authority on how many bytes we hold.
    return this.segments.map((segment) => ({ ...segment, bytesPulled: this.bytesOnDisk(segment.seq) }));
  }

  saveSegments(segments: SegmentRecord[]): void {
    this.saves += 1;
    this.segments = segments.map((segment) => ({ ...segment }));
  }
}

function run(engine: SyncEngine, onProgress: (progress: SyncProgress) => void = () => {}) {
  return engine.run(onProgress);
}

test('pulls every segment and stores exactly the bytes the device holds', async () => {
  const content = new Map([
    [4, segmentContent(4, 6)],
    [5, segmentContent(5, 6)],
    [6, segmentContent(6, 3)],
  ]);
  const device = new FakeDevice({ content, segmentBytes: 6 * SD_BLE_SIZE });
  const store = new MemoryStore();

  const result = await run(new SyncEngine(device, store));

  assert.equal(result.cancelled, false);
  assert.equal(result.error, undefined);
  for (const [seq, expected] of content) {
    assert.deepEqual(store.files.get(seq), expected, `segment ${seq} is byte-identical`);
  }
  assert.equal(result.bytesPulled, 15 * SD_BLE_SIZE);
  assert.deepEqual(
    device.reads.filter((read) => (read.segment & SEGMENT_INDEX_FLAG) === 0).map((read) => read.offset),
    [0, 0, 0],
    'a fresh sync starts every segment at zero',
  );
});

test('resumes after a cancel without duplicating or losing a byte', async () => {
  // Long enough that the progress ticker fires while blocks are still arriving,
  // which is the only place a cancel can be noticed.
  const blocks = 1500;
  const content = new Map([[9, segmentContent(9, blocks)]]);
  const device = new FakeDevice({ content, segmentBytes: blocks * SD_BLE_SIZE });
  const store = new MemoryStore();

  const first = new SyncEngine(device, store);
  const cancelled = await run(first, (progress) => {
    if (progress.phase === 'pulling') {
      first.cancel();
    }
  });

  assert.equal(cancelled.cancelled, true);
  const partial = store.bytesOnDisk(9);
  assert.ok(partial > 0 && partial < blocks * SD_BLE_SIZE, `stopped mid-segment, got ${partial} bytes`);
  assert.equal(partial % SD_BLE_SIZE, 0, 'a partial sync still leaves a block-aligned file');

  const second = await run(new SyncEngine(device, store));

  assert.equal(second.cancelled, false);
  assert.deepEqual(store.files.get(9), content.get(9), 'resumed file matches the device exactly');
  const audioReads = device.reads.filter((read) => (read.segment & SEGMENT_INDEX_FLAG) === 0);
  assert.equal(
    audioReads[audioReads.length - 1].offset,
    partial,
    'the second pass asks for exactly what is missing',
  );
});

test('ignores blocks that arrive after STOP', async () => {
  const content = new Map([
    [1, segmentContent(1, 4)],
    [2, segmentContent(2, 4)],
  ]);
  const device = new FakeDevice({ content, segmentBytes: 4 * SD_BLE_SIZE, stragglerBlocks: 3 });
  const store = new MemoryStore();

  await run(new SyncEngine(device, store));

  // Stragglers are 0xEE filler; if the settle window failed they would be
  // appended to whichever segment was next.
  assert.deepEqual(store.files.get(1), content.get(1));
  assert.deepEqual(store.files.get(2), content.get(2));
});

test('holds back the growing segment\u2019s partial block until the rest arrives', async () => {
  const full = segmentContent(3, 5, 137);
  const device = new FakeDevice({
    content: new Map([[3, full.subarray(0, 2 * SD_BLE_SIZE + 100)]]),
    segmentBytes: 10 * SD_BLE_SIZE,
  });
  const store = new MemoryStore();

  await run(new SyncEngine(device, store));
  assert.equal(store.bytesOnDisk(3), 2 * SD_BLE_SIZE, 'the 100-byte tail of a growing segment is not stored');

  // The device keeps recording; the segment is now sealed at its final length.
  device.update({ content: new Map([[3, full], [4, segmentContent(4, 1)]]) });
  await run(new SyncEngine(device, store));

  assert.deepEqual(store.files.get(3), full, 'sealed segment ends up complete, tail included');
});

test('marks a segment the device has rotated past as evicted', async () => {
  const store = new MemoryStore();
  store.segments = upsertSegment([], { seq: 1, bytesPulled: SD_BLE_SIZE, deviceBytes: SD_BLE_SIZE });
  const device = new FakeDevice({
    content: new Map([[7, segmentContent(7, 2)]]),
    segmentBytes: 2 * SD_BLE_SIZE,
  });

  const result = await run(new SyncEngine(device, store));

  assert.equal(result.segments.find((segment) => segment.seq === 1)?.evicted, true);
  assert.equal(
    store.segments.find((segment) => segment.seq === 1)?.evicted,
    true,
    'the eviction is persisted, not just reported',
  );
});

test('stores the timestamp sidecar alongside the audio', async () => {
  const idx = new Uint8Array(16);
  idx.set([0, 0, 0, 0, 0x40, 0xe2, 0x01, 0x00]); // offset 0, epoch 123456
  const device = new FakeDevice({
    content: new Map([[2, segmentContent(2, 2)]]),
    segmentBytes: 2 * SD_BLE_SIZE,
    index: new Map([[2, idx]]),
  });
  const store = new MemoryStore();

  const result = await run(new SyncEngine(device, store));

  assert.deepEqual(store.indexFiles.get(2), idx);
  assert.deepEqual(result.segments.find((segment) => segment.seq === 2)?.indexRecords, [
    { offset: 0, epoch: 123456, uptimeSeconds: 0, bootId: 0 },
  ]);
});

test('stops asking for the sidecar once firmware rejects it', async () => {
  const device = new FakeDevice({
    content: new Map([
      [1, segmentContent(1, 2)],
      [2, segmentContent(2, 2)],
      [3, segmentContent(3, 2)],
    ]),
    segmentBytes: 2 * SD_BLE_SIZE,
    indexStatus: StorageStatus.InvalidCommand,
  });
  const store = new MemoryStore();

  await run(new SyncEngine(device, store));

  const indexReads = device.reads.filter((read) => (read.segment & SEGMENT_INDEX_FLAG) !== 0);
  assert.equal(indexReads.length, 1, 'one rejection is enough to stop trying for the whole run');
});

test('does not re-fetch a sidecar it already has', async () => {
  const idx = new Uint8Array(16);
  const device = new FakeDevice({
    content: new Map([[1, segmentContent(1, 2)]]),
    segmentBytes: 4 * SD_BLE_SIZE,
    index: new Map([[1, idx]]),
  });
  const store = new MemoryStore();

  await run(new SyncEngine(device, store));
  device.update({ content: new Map([[1, segmentContent(1, 4)]]) });
  await run(new SyncEngine(device, store));

  const indexReads = device.reads.filter((read) => (read.segment & SEGMENT_INDEX_FLAG) !== 0);
  assert.equal(indexReads.length, 1);
});

test('reports nothing to do when the device has nothing new', async () => {
  const device = new FakeDevice({
    content: new Map([[1, segmentContent(1, 2)]]),
    segmentBytes: 2 * SD_BLE_SIZE,
  });
  const store = new MemoryStore();

  await run(new SyncEngine(device, store));
  const readsAfterFirst = device.reads.length;

  const progress: SyncProgress[] = [];
  const second = await run(new SyncEngine(device, store), (update) => progress.push(update));

  assert.equal(second.bytesPulled, 0);
  assert.equal(device.reads.length, readsAfterFirst, 'no further reads are issued');
  assert.equal(progress[progress.length - 1].message, 'Already up to date');
});

/**
 * Pulls SD-card audio off the device, resuming wherever the last session ended.
 *
 * Resume works entirely client-side. The device does persist its own read cursor
 * and reports it in the info characteristic, but that is a single global value
 * with no segment number attached, so it becomes meaningless after any rotation.
 * Instead we count bytes ourselves: because blocks are served strictly
 * sequentially from the requested offset, the next offset is just the size of
 * what we already wrote to disk.
 */

import {
  SD_BLE_SIZE,
  StorageCommand,
  StorageStatus,
  TRANSFER_STARTUP_MS,
} from '../ble/constants';
import {
  StorageCommandError,
  indexSegment,
  parseIndexRecords,
  segmentNumberForSeq,
  type IndexRecord,
  type RingInfo,
  type StorageNotification,
} from '../ble/protocol';
import { formatBytes } from '../ui/format';
import { buildPlan, writableLength } from './plan';
import { upsertSegment, type SegmentRecord } from './segments';

/** No new bytes for this long ends a transfer. Matches the reference client. */
const STALL_MS = 10_000;

/** Buffer this much before touching the filesystem, so we never hold a whole segment in RAM. */
const FLUSH_BYTES = 256 * 1024;

const PROGRESS_TICK_MS = 400;

/**
 * Blocks already queued when we send STOP still arrive afterwards. Waiting with
 * no transfer registered lets those stragglers be discarded, instead of landing
 * in the next transfer's buffer.
 */
const SETTLE_AFTER_STOP_MS = 300;

const delay = (ms: number) => new Promise<void>((resolve) => setTimeout(resolve, ms));

export type SyncPhase =
  | 'idle'
  | 'reading-info'
  | 'waiting'
  | 'pulling'
  | 'finishing'
  | 'done'
  | 'cancelled'
  | 'error';

export interface SyncProgress {
  phase: SyncPhase;
  message: string;
  /** Sequence number currently being pulled. */
  currentSeq: number | null;
  segmentsDone: number;
  segmentsTotal: number;
  bytesPulled: number;
  bytesTarget: number;
  kbps: number;
}

export interface SyncResult {
  bytesPulled: number;
  segments: SegmentRecord[];
  cancelled: boolean;
  error?: string;
}

type TransferEnd = 'eof' | 'length' | 'stall' | 'cancelled' | 'status';

interface ActiveTransfer {
  onData: (bytes: Uint8Array) => void;
  onStatus: (status: number) => void;
}

/** The part of `OmiClient` the engine uses. Narrow enough to fake in a test. */
export interface SyncClient {
  readonly id: string;
  readInfo(): Promise<RingInfo>;
  subscribe(
    onNotification: (notification: StorageNotification) => void,
    onError: (error: Error) => void,
  ): void;
  unsubscribe(): void;
  sendCommand(command: number, segment: number, offset?: number): Promise<void>;
  stopTransfer(segment: number): Promise<void>;
}

/** Persistence the engine needs. `./fileStore` is the real implementation. */
export interface SyncStore {
  bytesOnDisk(seq: number): number;
  appendToSegment(seq: number, bytes: Uint8Array): void;
  writeIndexFile(seq: number, bytes: Uint8Array): void;
  loadSegments(): SegmentRecord[];
  saveSegments(segments: SegmentRecord[]): void;
}

export class SyncEngine {
  private readonly client: SyncClient;

  private readonly store: SyncStore;

  private active: ActiveTransfer | null = null;

  private cancelled = false;

  private running = false;

  /**
   * Firmware without the timestamp index never answers an index read, which
   * costs a full stall timeout. One stall is enough to stop asking.
   */
  private indexSupported = true;

  constructor(client: SyncClient, store: SyncStore) {
    this.client = client;
    this.store = store;
  }

  cancel(): void {
    this.cancelled = true;
  }

  get isRunning(): boolean {
    return this.running;
  }

  private handleNotification = (notification: StorageNotification): void => {
    if (!this.active) {
      return;
    }
    if (notification.kind === 'data') {
      this.active.onData(notification.bytes);
    } else {
      this.active.onStatus(notification.status);
    }
  };

  /**
   * Runs one sync pass over everything the device still holds that we do not.
   * Safe to call again after a cancel or a dropped connection: progress is on
   * disk, so the next pass simply starts where this one stopped.
   */
  async run(onProgress: (progress: SyncProgress) => void): Promise<SyncResult> {
    this.cancelled = false;
    this.running = true;

    let segments = this.store.loadSegments();
    let bytesPulled = 0;
    let subscribed = false;
    const startedAt = Date.now();

    const report = (partial: Partial<SyncProgress> & { phase: SyncPhase; message: string }) => {
      const elapsed = (Date.now() - startedAt) / 1000;
      onProgress({
        currentSeq: null,
        segmentsDone: 0,
        segmentsTotal: 0,
        bytesPulled,
        bytesTarget: 0,
        kbps: elapsed > 0 ? bytesPulled / 1024 / elapsed : 0,
        ...partial,
      });
    };

    try {
      report({ phase: 'reading-info', message: 'Reading device storage info' });
      const info = await this.client.readInfo();

      segments = markEvicted(segments, info);
      this.store.saveSegments(segments);

      const plan = buildPlan(info, (seq) => this.store.bytesOnDisk(seq));
      const bytesTarget = plan.reduce((sum, item) => sum + item.remaining, 0);
      const segmentsTotal = plan.length;

      if (segmentsTotal === 0) {
        report({ phase: 'done', message: 'Already up to date', segmentsTotal: 0, bytesTarget: 0 });
        return { bytesPulled: 0, segments, cancelled: false };
      }

      this.client.subscribe(this.handleNotification, () => {
        // Monitor errors surface as a stall, which the transfer loop already
        // handles. Nothing extra to do here.
      });
      subscribed = true;

      let segmentsDone = 0;

      for (const item of plan) {
        if (this.cancelled) {
          break;
        }

        const segmentNumber = segmentNumberForSeq(info, item.seq);
        if (segmentNumber === null) {
          // Evicted between planning and now.
          segments = upsertSegment(segments, { seq: item.seq, evicted: true });
          segmentsDone += 1;
          continue;
        }

        report({
          phase: 'waiting',
          message: `Opening segment ${item.seq} on the device`,
          currentSeq: item.seq,
          segmentsDone,
          segmentsTotal,
          bytesTarget,
        });

        let pending: Uint8Array[] = [];
        let pendingBytes = 0;

        const sealed = item.seq < info.newestSeq;

        const flush = (final: boolean) => {
          if (pendingBytes === 0) {
            return;
          }
          const merged = mergeChunks(pending, pendingBytes);
          const writable = writableLength(merged.length, sealed, final);

          if (writable > 0) {
            this.store.appendToSegment(item.seq, merged.subarray(0, writable));
          }

          if (final) {
            pending = [];
            pendingBytes = 0;
            return;
          }
          const remainder = merged.subarray(writable);
          pending = remainder.length > 0 ? [remainder] : [];
          pendingBytes = remainder.length;
        };

        const outcome = await this.pull({
          segment: segmentNumber,
          start: item.start,
          needed: item.remaining,
          onData: (bytes) => {
            pending.push(bytes);
            pendingBytes += bytes.length;
            bytesPulled += bytes.length;
            if (pendingBytes >= FLUSH_BYTES) {
              flush(false);
            }
          },
          onTick: () => {
            report({
              phase: 'pulling',
              message: `Pulling segment ${item.seq}`,
              currentSeq: item.seq,
              segmentsDone,
              segmentsTotal,
              bytesTarget,
            });
          },
        });

        flush(true);
        await this.client.stopTransfer(segmentNumber);
        await delay(SETTLE_AFTER_STOP_MS);

        // A segment the device reports as empty, or an offset already at the
        // end, means there is simply nothing more to take -- not a failure.
        if (
          outcome.end === 'status' &&
          outcome.status !== undefined &&
          outcome.status !== StorageStatus.EmptySegment &&
          outcome.status !== StorageStatus.OffsetPastEnd
        ) {
          throw new StorageCommandError(outcome.status);
        }

        const onDisk = this.store.bytesOnDisk(item.seq);
        segments = upsertSegment(segments, {
          seq: item.seq,
          deviceId: this.client.id,
          bytesPulled: onDisk,
          deviceBytes: item.total,
          complete: onDisk >= item.total,
        });
        this.store.saveSegments(segments);

        const alreadyLabelled = (segments.find((s) => s.seq === item.seq)?.indexRecords.length ?? 0) > 0;
        if (this.indexSupported && !alreadyLabelled && outcome.end !== 'cancelled' && onDisk > 0) {
          const records = await this.pullIndex(info, item.seq);
          if (records.length > 0) {
            segments = upsertSegment(segments, { seq: item.seq, indexRecords: records });
            this.store.saveSegments(segments);
          }
        }

        segmentsDone += 1;

        if (outcome.end === 'cancelled') {
          break;
        }
      }

      const cancelled = this.cancelled;
      report({
        phase: cancelled ? 'cancelled' : 'done',
        message: cancelled
          ? `Stopped. ${formatBytes(bytesPulled)} saved.`
          : `Synced ${formatBytes(bytesPulled)}`,
        segmentsDone,
        segmentsTotal,
        bytesTarget,
      });

      return { bytesPulled, segments, cancelled };
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      this.store.saveSegments(segments);
      report({ phase: 'error', message });
      return { bytesPulled, segments, cancelled: this.cancelled, error: message };
    } finally {
      this.active = null;
      this.running = false;
      if (subscribed) {
        this.client.unsubscribe();
      }
    }
  }

  /**
   * Issues one READ and collects blocks until the device signals end of file,
   * we have what we asked for, the link stalls, or the user cancels.
   */
  private pull(params: {
    segment: number;
    start: number;
    needed: number;
    onData: (bytes: Uint8Array) => void;
    onTick: () => void;
  }): Promise<{ end: TransferEnd; received: number; status?: number }> {
    const { segment, start, needed, onData, onTick } = params;

    return new Promise((resolve) => {
      let received = 0;
      let lastDataAt = Date.now();
      let settled = false;
      let timer: ReturnType<typeof setInterval> | null = null;

      const finish = (end: TransferEnd, status?: number) => {
        if (settled) {
          return;
        }
        settled = true;
        if (timer) {
          clearInterval(timer);
        }
        this.active = null;
        resolve({ end, received, status });
      };

      this.active = {
        onData: (bytes) => {
          received += bytes.length;
          lastDataAt = Date.now();
          onData(bytes);
          if (received >= needed) {
            finish('length');
          }
        },
        onStatus: (status) => {
          if (status === StorageStatus.EndOfTransfer) {
            finish('eof');
          } else if (status !== StorageStatus.Ok && status !== StorageStatus.Deleted) {
            finish('status', status);
          }
        },
      };

      timer = setInterval(() => {
        if (this.cancelled) {
          finish('cancelled');
          return;
        }
        // The firmware sleeps ~1.5 s between accepting a READ and sending the
        // first block, so give the opening move extra room before calling it
        // stalled.
        const grace = received === 0 ? STALL_MS + TRANSFER_STARTUP_MS : STALL_MS;
        if (Date.now() - lastDataAt > grace) {
          finish('stall');
          return;
        }
        onTick();
      }, PROGRESS_TICK_MS);

      this.client.sendCommand(StorageCommand.Read, segment, start).catch(() => {
        finish('stall');
      });
    });
  }

  /**
   * Fetches a segment's timestamp sidecar over the same transfer path. Purely
   * cosmetic -- it turns "segment 12, part 3" into a real date -- so any failure
   * is swallowed.
   */
  private async pullIndex(info: RingInfo, seq: number): Promise<IndexRecord[]> {
    const segmentNumber = segmentNumberForSeq(info, seq);
    if (segmentNumber === null) {
      return [];
    }

    const chunks: Uint8Array[] = [];
    let total = 0;

    const outcome = await this.pull({
      segment: indexSegment(segmentNumber),
      start: 0,
      needed: Number.MAX_SAFE_INTEGER,
      onData: (bytes) => {
        chunks.push(bytes);
        total += bytes.length;
      },
      onTick: () => {},
    });

    await this.client.stopTransfer(segmentNumber);
    await delay(SETTLE_AFTER_STOP_MS);

    if (outcome.end === 'stall' || outcome.end === 'status') {
      this.indexSupported = false;
      return [];
    }
    if (total === 0) {
      return [];
    }

    const merged = mergeChunks(chunks, total);
    this.store.writeIndexFile(seq, merged);
    return parseIndexRecords(merged);
  }
}

/** Anything the device has rotated past can never be completed. Say so rather than retrying. */
function markEvicted(segments: SegmentRecord[], info: RingInfo): SegmentRecord[] {
  let next = segments;
  for (const segment of segments) {
    if (segment.seq < info.oldestSeq && !segment.evicted) {
      next = upsertSegment(next, { seq: segment.seq, evicted: true });
    }
  }
  return next;
}

function mergeChunks(chunks: Uint8Array[], total: number): Uint8Array {
  const merged = new Uint8Array(total);
  let at = 0;
  for (const chunk of chunks) {
    merged.set(chunk, at);
    at += chunk.length;
  }
  return merged;
}

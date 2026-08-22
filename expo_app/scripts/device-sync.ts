/**
 * Runs the app's real sync engine against a real DevKit v2, from a laptop.
 *
 * This exists because BLE needs hardware the iOS Simulator does not have. It
 * substitutes only the two things the phone would provide -- the BLE transport
 * (via scripts/ble-bridge.py) and the filesystem -- and runs the production
 * `SyncEngine` above them, so the resume plan, block accounting, stall
 * handling, STOP settling and `.idx` fetch are all the shipped code paths.
 *
 * Usage:
 *   npx tsx scripts/device-sync.ts --dir /tmp/omi-sync             # sync until done
 *   npx tsx scripts/device-sync.ts --dir /tmp/omi-sync --stop 45   # cancel after 45s
 *   npx tsx scripts/device-sync.ts --dir /tmp/omi-sync --decode    # decode what is on disk
 *   npx tsx scripts/device-sync.ts --dir /tmp/omi-sync --trace     # log every command and status
 *   npx tsx scripts/device-sync.ts --dir /tmp/omi-sync --skip-seq 13,14
 *
 * Needs bleak. By default it uses the venv the reference client already has:
 *   omi/firmware/scripts/devkit/sd_sync/.venv/bin/python
 */

import { execFileSync, spawn, spawnSync, type ChildProcessWithoutNullStreams } from 'node:child_process';
import { appendFileSync, existsSync, mkdirSync, readFileSync, statSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { createInterface } from 'node:readline';

import { muxOggOpus } from '../src/audio/oggOpus';
import { framesToSeconds, parseOpusFrames } from '../src/audio/opusFrames';
import { SD_BLE_SIZE, StorageCommand, describeStatus } from '../src/ble/constants';
import {
  describeSecurityError,
  diagnosePairing,
  parsePairingStatus,
  type PairingStatus,
} from '../src/ble/pairing';
import {
  encodeCommand,
  parseRingInfo,
  totalRingBytes,
  type RingInfo,
  type StorageNotification,
} from '../src/ble/protocol';
import { parseManifest, serializeManifest, type SegmentRecord } from '../src/sync/segments';
import { SyncEngine, type SyncClient, type SyncStore } from '../src/sync/syncEngine';
import { formatBytes } from '../src/ui/format';

const DEFAULT_PYTHON = join(
  __dirname,
  '..',
  '..',
  'omi',
  'firmware',
  'scripts',
  'devkit',
  'sd_sync',
  '.venv',
  'bin',
  'python',
);

interface Args {
  dir: string;
  stopAfterSeconds: number | null;
  python: string;
  decodeOnly: boolean;
  trace: boolean;
  skipSeqs: number[];
}

function parseArgs(argv: string[]): Args {
  const value = (flag: string) => {
    const at = argv.indexOf(flag);
    return at === -1 ? null : argv[at + 1];
  };
  return {
    dir: value('--dir') ?? '/tmp/omi-sync',
    stopAfterSeconds: value('--stop') ? Number(value('--stop')) : null,
    python: value('--python') ?? DEFAULT_PYTHON,
    decodeOnly: argv.includes('--decode'),
    trace: argv.includes('--trace'),
    skipSeqs: (value('--skip-seq') ?? '').split(',').filter(Boolean).map(Number),
  };
}

/** `SyncClient` backed by the Python BLE bridge. */
class BridgeClient implements SyncClient {
  readonly id: string;

  readonly mtu: number;

  private readonly child: ChildProcessWithoutNullStreams;

  private notify: ((notification: StorageNotification) => void) | null = null;

  private nextId = 1;

  trace = false;

  private readonly pending = new Map<number, { resolve: (data: Uint8Array) => void; reject: (error: Error) => void }>();

  private constructor(child: ChildProcessWithoutNullStreams, id: string, mtu: number) {
    this.child = child;
    this.id = id;
    this.mtu = mtu;
  }

  static async open(python: string): Promise<BridgeClient> {
    const script = join(__dirname, 'ble-bridge.py');
    const child = spawn(python, ['-u', script], { stdio: ['pipe', 'pipe', 'pipe'] });
    child.stderr.on('data', (chunk) => process.stderr.write(`  [bridge] ${chunk}`));

    return new Promise((resolve, reject) => {
      let client: BridgeClient | null = null;

      createInterface({ input: child.stdout }).on('line', (line) => {
        let event: Record<string, unknown>;
        try {
          event = JSON.parse(line);
        } catch {
          process.stderr.write(`  [bridge] ${line}\n`);
          return;
        }

        if (event.t === 'ready') {
          client = new BridgeClient(child, String(event.address), Number(event.mtu ?? 0));
          resolve(client);
          return;
        }
        if (!client) {
          if (event.t === 'error') {
            reject(new Error(String(event.message)));
          }
          return;
        }
        client.handle(event);
      });

      child.on('exit', (code) => {
        if (!client) {
          reject(new Error(`bridge exited with code ${code}`));
        }
      });
    });
  }

  private handle(event: Record<string, unknown>): void {
    if (event.t === 'notify') {
      const bytes = Uint8Array.from(Buffer.from(String(event.data), 'base64'));
      if (bytes.length === 1) {
        this.notify?.({ kind: 'status', status: bytes[0] });
      } else if (bytes.length > 1) {
        this.notify?.({ kind: 'data', bytes });
      }
      return;
    }

    if (event.t === 'error') {
      console.log(`      !! bridge error: ${String(event.message)}`);
    }

    const waiter = this.pending.get(Number(event.id));
    if (!waiter) {
      return;
    }
    this.pending.delete(Number(event.id));

    if (event.t === 'error') {
      waiter.reject(new Error(String(event.message)));
    } else if (event.t === 'info' || event.t === 'pairing') {
      waiter.resolve(Uint8Array.from(Buffer.from(String(event.data), 'base64')));
    } else {
      waiter.resolve(new Uint8Array(0));
    }
  }

  private request(payload: Record<string, unknown>): Promise<Uint8Array> {
    const id = this.nextId;
    this.nextId += 1;
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject });
      this.child.stdin.write(`${JSON.stringify({ ...payload, id })}\n`);
    });
  }

  async readInfo(): Promise<RingInfo> {
    return parseRingInfo(await this.request({ op: 'info' }));
  }

  async readPairingStatus(): Promise<PairingStatus | null> {
    try {
      return parsePairingStatus(await this.request({ op: 'pairing' }));
    } catch {
      // Firmware without the pairing service encrypts nothing, so there is nothing to report.
      return null;
    }
  }

  subscribe(onNotification: (notification: StorageNotification) => void): void {
    // The bridge subscribes once at connect time, as the app does; this only
    // routes the stream to the engine.
    this.notify = (notification) => {
      if (this.trace && notification.kind === 'status') {
        console.log(`      <- status ${notification.status} (${describeStatus(notification.status)})`);
      }
      onNotification(notification);
    };
  }

  unsubscribe(): void {
    this.notify = null;
  }

  async sendCommand(command: number, segment: number, offset = 0): Promise<void> {
    const frame = encodeCommand(command, segment, offset);
    if (this.trace) {
      console.log(`      -> cmd ${command} segment ${segment} offset ${offset}`);
    }
    await this.request({ op: 'write', data: Buffer.from(frame).toString('base64') });
  }

  async stopTransfer(segment: number): Promise<void> {
    try {
      await this.sendCommand(StorageCommand.Stop, Math.max(1, segment), 0);
    } catch {
      // Best effort, exactly as OmiClient treats it: the write often fails
      // while notifications still saturate the link.
    }
  }

  close(): void {
    this.child.stdin.write(`${JSON.stringify({ op: 'quit' })}\n`);
    this.child.stdin.end();
  }
}

/** `SyncStore` on the local filesystem, mirroring the on-phone layout. */
class DiskStore implements SyncStore {
  /**
   * Sequence numbers to report as already complete, so the planner skips them.
   * Only useful for working around a device that cannot serve some segment.
   */
  skipSeqs = new Set<number>();

  constructor(private readonly dir: string) {
    mkdirSync(dir, { recursive: true });
  }

  segmentPath(seq: number): string {
    return join(this.dir, `segment-${seq}.bin`);
  }

  private indexPath(seq: number): string {
    return join(this.dir, `index-${seq}.idx`);
  }

  private manifestPath(): string {
    return join(this.dir, 'manifest.json');
  }

  bytesOnDisk(seq: number): number {
    if (this.skipSeqs.has(seq)) {
      return Number.MAX_SAFE_INTEGER;
    }
    const path = this.segmentPath(seq);
    return existsSync(path) ? statSync(path).size : 0;
  }

  appendToSegment(seq: number, bytes: Uint8Array): void {
    appendFileSync(this.segmentPath(seq), bytes);
  }

  writeIndexFile(seq: number, bytes: Uint8Array): void {
    writeFileSync(this.indexPath(seq), bytes);
  }

  loadSegments(): SegmentRecord[] {
    const path = this.manifestPath();
    if (!existsSync(path)) {
      return [];
    }
    return parseManifest(readFileSync(path, 'utf8'), (seq) => this.bytesOnDisk(seq));
  }

  saveSegments(segments: SegmentRecord[]): void {
    writeFileSync(this.manifestPath(), serializeManifest(segments));
  }
}

function describeRing(info: RingInfo): string {
  return [
    `${info.count} segments on card, sequence ${info.oldestSeq}..${info.newestSeq}`,
    `newest holds ${formatBytes(info.newestBytes)}`,
    `${formatBytes(totalRingBytes(info))} retained`,
  ].join(', ');
}

/** Muxes what is on disk into Ogg and lets ffmpeg judge whether it is real audio. */
function decodeOnDisk(store: DiskStore, segments: SegmentRecord[]): void {
  for (const segment of segments) {
    const path = store.segmentPath(segment.seq);
    if (!existsSync(path)) {
      continue;
    }
    const raw = new Uint8Array(readFileSync(path));
    const { frames, skipped } = parseOpusFrames(raw);
    if (frames.length === 0) {
      console.log(`  segment ${segment.seq}: no frames recovered from ${formatBytes(raw.length)}`);
      continue;
    }

    const opusPath = `${path}.opus`;
    const wavPath = `${path}.wav`;
    writeFileSync(opusPath, muxOggOpus(frames));
    execFileSync('ffmpeg', ['-y', '-v', 'error', '-i', opusPath, wavPath]);

    const probe = execFileSync('ffprobe', [
      '-v', 'error',
      '-show_entries', 'format=duration:stream=codec_name,channels,sample_rate',
      '-of', 'default=nw=1:nk=1',
      opusPath,
    ]).toString().trim().split('\n');

    // volumedetect reports on stderr, which is where ffmpeg puts all diagnostics.
    const volume = spawnSync('ffmpeg', ['-v', 'info', '-i', wavPath, '-af', 'volumedetect', '-f', 'null', '-']);
    const stats = /mean_volume: (\S+ dB)[\s\S]*?max_volume: (\S+ dB)/.exec(volume.stderr.toString()) ?? [];

    console.log(
      `  segment ${segment.seq}: ${formatBytes(raw.length)} -> ${frames.length.toLocaleString()} frames ` +
        `(${((skipped / Math.max(raw.length, 1)) * 100).toFixed(1)}% resync skips), ` +
        `${framesToSeconds(frames.length).toFixed(1)}s`,
    );
    console.log(`      ffprobe ${probe.join(' / ')}`);
    if (stats.length === 3) {
      console.log(`      level   mean ${stats[1]}, peak ${stats[2]}  -> ${wavPath}`);
    }
  }
}

async function main(): Promise<number> {
  const args = parseArgs(process.argv.slice(2));
  const store = new DiskStore(args.dir);
  store.skipSeqs = new Set(args.skipSeqs);

  if (args.decodeOnly) {
    console.log(`Decoding what is already in ${args.dir}`);
    decodeOnDisk(store, store.loadSegments());
    return 0;
  }

  console.log('Scanning for the DevKit (it only advertises once its SD card mounts)...');
  const client = await BridgeClient.open(args.python);
  client.trace = args.trace;
  console.log(`Connected to ${client.id}, ATT MTU ${client.mtu}`);

  // Same order the app uses: the status read never needs encryption, so it is the one thing that
  // still answers when the link is not paired, and it says why.
  const pairing = await client.readPairingStatus();
  if (pairing) {
    const verdict = diagnosePairing(pairing);
    console.log(
      `  pairing: ${verdict} (SMP ${pairing.smpEnabled ? 'on' : 'off'}, ` +
        `${pairing.bondCount}/${pairing.maxBonds} slots, link ` +
        `${pairing.linkEncrypted ? 'encrypted' : 'clear'})`,
    );
    if (pairing.lastSecurityError !== 0) {
      console.log(`  last security error: ${describeSecurityError(pairing.lastSecurityError)}`);
    }
    if (verdict !== 'ready' && verdict !== 'not-required') {
      // macOS pairs on demand when an encrypted read is attempted, exactly as iOS does, so the
      // useful thing here is to say which remedy applies rather than to retry blindly.
      console.log('  the link is not encrypted; the encrypted reads below will fail');
    }
  }

  const info = await client.readInfo();
  console.log(`  ${describeRing(info)}`);

  const before = store.loadSegments().map((segment) => `${segment.seq}:${segment.bytesPulled}`);
  console.log(`  on disk before: ${before.length ? before.join(' ') : 'nothing'}`);

  const engine = new SyncEngine(client, store);
  if (args.stopAfterSeconds !== null) {
    setTimeout(() => {
      console.log(`\n  --- cancelling after ${args.stopAfterSeconds}s ---`);
      engine.cancel();
    }, args.stopAfterSeconds * 1000);
  }

  let lastLine = '';
  const result = await engine.run((progress) => {
    const line =
      `  [${progress.phase}] ${progress.message} ` +
      `${formatBytes(progress.bytesPulled)}/${formatBytes(progress.bytesTarget)} ` +
      `at ${progress.kbps.toFixed(1)} KB/s`;
    if (line !== lastLine) {
      console.log(line);
      lastLine = line;
    }
  });

  console.log(`\nResult: ${formatBytes(result.bytesPulled)} pulled, cancelled=${result.cancelled}`);
  if (result.error) {
    console.log(`  error: ${result.error}`);
  }
  for (const segment of result.segments) {
    const onDisk = store.bytesOnDisk(segment.seq);
    console.log(
      `  seq ${segment.seq}: ${formatBytes(onDisk)} on disk` +
        `${onDisk % SD_BLE_SIZE === 0 ? ' (block-aligned)' : ` (MISALIGNED by ${onDisk % SD_BLE_SIZE})`}` +
        `${segment.complete ? ', complete' : ''}${segment.evicted ? ', evicted' : ''}` +
        `${segment.indexRecords.length ? `, ${segment.indexRecords.length} index records` : ''}`,
    );
  }

  client.close();
  return result.error ? 1 : 0;
}

main().then(
  (code) => process.exit(code),
  (error) => {
    console.error(error);
    process.exit(1);
  },
);

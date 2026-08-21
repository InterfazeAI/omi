/**
 * Writes a fixture recording in the on-card byte format, so the parse -> mux ->
 * decode -> play path can be exercised without a DevKit.
 *
 * The iOS Simulator has no Bluetooth LE radio, so this is the only way to test
 * playback there. Drop the output into the app's Documents directory:
 *
 *   npx tsx scripts/seed-recording.ts --out "$(xcrun simctl get_app_container \
 *     booted com.omi.expo.sync data)/Documents/recordings"
 *
 * Then relaunch the app; the recording shows up in the Recordings list.
 */

import { execFileSync } from 'node:child_process';
import { mkdirSync, mkdtempSync, readFileSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

import { SD_BLE_SIZE } from '../src/ble/constants';

const DEFAULT_SECONDS = 90;

function arg(name: string): string | null {
  const index = process.argv.indexOf(`--${name}`);
  return index === -1 ? null : (process.argv[index + 1] ?? null);
}

function concat(parts: Uint8Array[]): Uint8Array {
  const out = new Uint8Array(parts.reduce((sum, p) => sum + p.length, 0));
  let at = 0;
  for (const part of parts) {
    out.set(part, at);
    at += part.length;
  }
  return out;
}

/** Pulls the raw Opus packets out of an Ogg stream. */
function demuxOgg(bytes: Uint8Array): Uint8Array[] {
  const packets: Uint8Array[] = [];
  let pending: Uint8Array[] = [];
  let at = 0;

  while (at + 27 <= bytes.length) {
    const segmentCount = bytes[at + 26];
    const table = bytes.subarray(at + 27, at + 27 + segmentCount);
    let payloadAt = at + 27 + segmentCount;
    const start = payloadAt;
    let payloadLength = 0;

    for (const lacing of table) {
      pending.push(bytes.subarray(payloadAt, payloadAt + lacing));
      payloadAt += lacing;
      payloadLength += lacing;
      if (lacing < 255) {
        packets.push(concat(pending));
        pending = [];
      }
    }
    at = start + payloadLength;
  }

  return packets.filter((packet) => {
    const magic = String.fromCharCode(...packet.subarray(0, 8));
    return magic !== 'OpusHead' && magic !== 'OpusTags';
  });
}

/** Mirrors transport.c write_to_storage(), stale block tails and all. */
function packLikeFirmware(frames: Uint8Array[]): Uint8Array {
  const blocks: Uint8Array[] = [];
  let stale = 0x11;
  let block = new Uint8Array(SD_BLE_SIZE).fill(stale);
  let offset = 0;

  const nextBlock = () => {
    blocks.push(block);
    stale = (stale + 0x37) & 0xff;
    block = new Uint8Array(SD_BLE_SIZE).fill(stale);
  };

  for (const frame of frames) {
    const packetSize = frame.length + 1;
    if (offset + packetSize > SD_BLE_SIZE - 1) {
      block[offset] = frame.length;
      nextBlock();
      block[0] = frame.length;
      block.set(frame, 1);
      offset = packetSize;
    } else {
      block[offset] = frame.length;
      block.set(frame, offset + 1);
      offset += packetSize;
    }
  }
  nextBlock();

  return concat(blocks);
}

function main(): void {
  const outDir = arg('out');
  if (!outDir) {
    throw new Error('pass --out <recordings directory>');
  }
  const seconds = Number(arg('seconds') ?? DEFAULT_SECONDS);
  const seq = Number(arg('seq') ?? 1);

  const workDir = mkdtempSync(join(tmpdir(), 'omi-seed-'));
  const referencePath = join(workDir, 'reference.opus');

  console.log(`Encoding ${seconds}s of test audio with the firmware's Opus settings`);
  execFileSync('ffmpeg', [
    '-y', '-loglevel', 'error',
    '-f', 'lavfi',
    // A sweep rather than a flat tone, so it is obvious by ear whether playback
    // is running forwards, looping, or resuming at the right offset.
    '-i', `sine=frequency=220:duration=${seconds}:sample_rate=16000`,
    '-af', 'vibrato=f=0.2:d=0.9,volume=0.6',
    '-c:a', 'libopus',
    '-b:a', '20k',
    '-frame_duration', '10',
    '-application', 'lowdelay',
    '-ac', '1',
    referencePath,
  ]);

  const frames = demuxOgg(new Uint8Array(readFileSync(referencePath)));
  const raw = packLikeFirmware(frames);

  mkdirSync(outDir, { recursive: true });
  writeFileSync(join(outDir, `segment-${seq}.bin`), raw);

  const startedAt = Math.floor(Date.now() / 1000) - seconds;
  const manifest = {
    version: 1,
    segments: [
      {
        seq,
        deviceId: 'fixture',
        bytesPulled: raw.length,
        deviceBytes: raw.length,
        complete: true,
        evicted: false,
        indexRecords: [{ offset: 0, epoch: startedAt, uptimeSeconds: 0, bootId: 1 }],
        updatedAt: Date.now(),
      },
    ],
  };
  writeFileSync(join(outDir, 'manifest.json'), JSON.stringify(manifest));

  console.log(
    `Wrote ${frames.length} frames as ${raw.length} bytes ` +
      `(${raw.length / SD_BLE_SIZE} blocks) to ${outDir}`,
  );
}

main();

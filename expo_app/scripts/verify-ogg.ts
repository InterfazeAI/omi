/**
 * End-to-end check of the frame parser and the Ogg muxer, without a device.
 *
 *   1. ffmpeg encodes a known signal with the firmware's Opus settings.
 *   2. We demux it to get ground-truth Opus packets.
 *   3. We repack them exactly the way transport.c write_to_storage() does,
 *      including the stray boundary length byte and the uncleaned block tail.
 *   4. parseOpusFrames() must recover every original packet byte for byte.
 *   5. muxOggOpus() must produce a container ffmpeg decodes to the same PCM.
 *
 * Step 5 is the one that matters: react-native-audio-api decodes .opus through
 * libopusfile, so if our container is malformed, playback fails on device. This
 * catches that on a laptop.
 *
 * Usage:
 *   npx tsx scripts/verify-ogg.ts                # synthetic signal via ffmpeg
 *   npx tsx scripts/verify-ogg.ts --raw dump.bin # a real dump from the device
 *
 * A real dump can be captured with the reference client:
 *   python3 omi/firmware/scripts/devkit/sd_sync/pull_range.py
 */

import { execFileSync } from 'node:child_process';
import { mkdtempSync, readFileSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

import { SD_BLE_SIZE } from '../src/ble/constants';
import { muxOggOpus } from '../src/audio/oggOpus';
import { framesToSeconds, parseOpusFrames } from '../src/audio/opusFrames';

const workDir = mkdtempSync(join(tmpdir(), 'omi-ogg-'));

let failures = 0;

function check(label: string, ok: boolean, detail = ''): void {
  if (ok) {
    console.log(`  PASS  ${label}${detail ? ` (${detail})` : ''}`);
  } else {
    failures += 1;
    console.error(`  FAIL  ${label}${detail ? ` (${detail})` : ''}`);
  }
}

function run(command: string, args: string[]): Buffer {
  return execFileSync(command, args, { maxBuffer: 512 * 1024 * 1024 });
}

/** Splits an Ogg stream into packets, following lacing and page continuation. */
function demuxOgg(bytes: Uint8Array): Uint8Array[] {
  const packets: Uint8Array[] = [];
  let pending: Uint8Array[] = [];
  let at = 0;

  while (at + 27 <= bytes.length) {
    if (
      bytes[at] !== 0x4f ||
      bytes[at + 1] !== 0x67 ||
      bytes[at + 2] !== 0x67 ||
      bytes[at + 3] !== 0x53
    ) {
      throw new Error(`lost Ogg sync at byte ${at}`);
    }

    const segmentCount = bytes[at + 26];
    const table = bytes.subarray(at + 27, at + 27 + segmentCount);
    let payloadAt = at + 27 + segmentCount;
    const pageStart = payloadAt;
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

    at = pageStart + payloadLength;
  }

  return packets;
}

function concat(parts: Uint8Array[]): Uint8Array {
  const total = parts.reduce((sum, p) => sum + p.length, 0);
  const out = new Uint8Array(total);
  let at = 0;
  for (const part of parts) {
    out.set(part, at);
    at += part.length;
  }
  return out;
}

function isHeaderPacket(packet: Uint8Array): boolean {
  const magic = String.fromCharCode(...packet.subarray(0, 8));
  return magic === 'OpusHead' || magic === 'OpusTags';
}

/**
 * Mirrors transport.c write_to_storage(). The stale fill matters: the firmware
 * never clears the tail of its block buffer, so a parser that trusts alignment
 * will read garbage there.
 */
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

function decodeToPcm(path: string): Int16Array {
  const raw = run('ffmpeg', [
    '-loglevel', 'error',
    '-i', path,
    '-f', 's16le',
    '-acodec', 'pcm_s16le',
    '-ar', '48000',
    '-ac', '1',
    '-',
  ]);
  return new Int16Array(raw.buffer, raw.byteOffset, Math.floor(raw.length / 2));
}

function probe(path: string): Record<string, string> {
  const out = run('ffprobe', [
    '-v', 'error',
    '-select_streams', 'a:0',
    '-show_entries', 'stream=codec_name,channels,sample_rate:format=duration',
    '-of', 'default=noprint_wrappers=1',
    path,
  ]).toString();

  const fields: Record<string, string> = {};
  for (const line of out.trim().split('\n')) {
    const [key, value] = line.split('=');
    fields[key] = value;
  }
  return fields;
}

function main(): void {
  const rawArgIndex = process.argv.indexOf('--raw');
  let rawStream: Uint8Array;
  let groundTruth: Uint8Array[] | null = null;
  let referencePath: string | null = null;

  if (rawArgIndex !== -1) {
    const path = process.argv[rawArgIndex + 1];
    if (!path) {
      throw new Error('--raw needs a file path');
    }
    console.log(`Using real device dump: ${path}`);
    rawStream = new Uint8Array(readFileSync(path));
  } else {
    console.log('Generating a reference signal with ffmpeg (firmware Opus settings)');
    referencePath = join(workDir, 'reference.opus');
    run('ffmpeg', [
      '-y', '-loglevel', 'error',
      '-f', 'lavfi',
      '-i', 'sine=frequency=440:duration=10:sample_rate=16000',
      '-c:a', 'libopus',
      '-b:a', '20k',
      '-frame_duration', '10',
      '-application', 'lowdelay',
      '-ac', '1',
      '-vbr', 'on',
      referencePath,
    ]);

    groundTruth = demuxOgg(new Uint8Array(readFileSync(referencePath))).filter(
      (p) => !isHeaderPacket(p),
    );
    console.log(`  ${groundTruth.length} reference Opus packets`);
    rawStream = packLikeFirmware(groundTruth);
    console.log(`  packed into ${rawStream.length / SD_BLE_SIZE} blocks of ${SD_BLE_SIZE} bytes`);
  }

  console.log('\nParsing frames');
  const { frames, skipped } = parseOpusFrames(rawStream);
  console.log(
    `  ${frames.length} frames, ${skipped} bytes skipped ` +
      `(${((skipped / rawStream.length) * 100).toFixed(1)}% of the stream), ` +
      `${framesToSeconds(frames.length).toFixed(1)}s of audio`,
  );

  check('recovered at least one frame', frames.length > 0);
  check(
    'resync skips stay within the expected few percent',
    skipped / Math.max(rawStream.length, 1) < 0.1,
    `${((skipped / rawStream.length) * 100).toFixed(1)}%`,
  );

  if (groundTruth) {
    const identical =
      frames.length === groundTruth.length &&
      frames.every((frame, i) => Buffer.compare(Buffer.from(frame), Buffer.from(groundTruth![i])) === 0);
    check(
      'every original packet recovered byte for byte',
      identical,
      `${frames.length}/${groundTruth.length}`,
    );
  }

  console.log('\nMuxing Ogg Opus');
  const ogg = muxOggOpus(frames);
  const oggPath = join(workDir, 'muxed.opus');
  writeFileSync(oggPath, ogg);
  console.log(`  ${ogg.length} bytes -> ${oggPath}`);

  const info = probe(oggPath);
  console.log(`  ffprobe: ${JSON.stringify(info)}`);
  check('ffprobe recognises the stream as opus', info.codec_name === 'opus', info.codec_name);
  check('mono', info.channels === '1', info.channels);
  check('48 kHz output rate', info.sample_rate === '48000', info.sample_rate);

  const expectedSeconds = framesToSeconds(frames.length);
  const reportedSeconds = Number(info.duration);
  check(
    'duration matches the frame count',
    Math.abs(reportedSeconds - expectedSeconds) < 0.05,
    `${reportedSeconds.toFixed(2)}s vs ${expectedSeconds.toFixed(2)}s`,
  );

  const ourPcm = decodeToPcm(oggPath);
  check('decodes to a non-empty waveform', ourPcm.length > 0, `${ourPcm.length} samples`);
  const peak = ourPcm.reduce((max, s) => Math.max(max, Math.abs(s)), 0);
  check('waveform is not silence', peak > 100, `peak ${peak}`);

  if (referencePath) {
    // ffmpeg writes pre-skip 120 and honours it on decode; we write 0 and keep
    // every sample, so our output leads theirs by exactly that many samples.
    const PRE_SKIP = 120;
    const theirPcm = decodeToPcm(referencePath);
    const comparable = Math.min(ourPcm.length - PRE_SKIP, theirPcm.length);
    let maxDelta = 0;
    for (let i = 0; i < comparable; i += 1) {
      maxDelta = Math.max(maxDelta, Math.abs(ourPcm[i + PRE_SKIP] - theirPcm[i]));
    }
    check(
      'decoded audio is sample-identical to the reference',
      maxDelta === 0 && comparable > 0,
      `${comparable} samples compared, max delta ${maxDelta}`,
    );
  }

  console.log(failures === 0 ? '\nAll checks passed.' : `\n${failures} check(s) failed.`);
  process.exit(failures === 0 ? 0 : 1);
}

main();

import assert from 'node:assert/strict';
import test from 'node:test';

import { OPUS_TOC, SD_BLE_SIZE } from '../ble/constants';
import { framesToSeconds, parseOpusFrames } from './opusFrames';

function frame(length: number, fill = 0x42): Uint8Array {
  const f = new Uint8Array(length);
  f[0] = OPUS_TOC;
  f.fill(fill, 1);
  return f;
}

/**
 * Reproduces transport.c write_to_storage(): frames are packed as [len][payload]
 * into 440-byte blocks, a frame that would overflow leaves a stray length byte at
 * the boundary and moves to the next block, and the tail of the buffer keeps
 * whatever the previous block left there.
 */
function packLikeFirmware(frames: Uint8Array[], staleFill = 0xcd): Uint8Array {
  const blocks: Uint8Array[] = [];
  let block = new Uint8Array(SD_BLE_SIZE).fill(staleFill);
  let offset = 0;

  for (const f of frames) {
    const packetSize = f.length + 1;
    if (offset + packetSize > SD_BLE_SIZE - 1) {
      block[offset] = f.length; // stray length byte, payload goes to the next block
      blocks.push(block);
      block = new Uint8Array(SD_BLE_SIZE).fill(staleFill);
      block[0] = f.length;
      block.set(f, 1);
      offset = packetSize;
    } else {
      block[offset] = f.length;
      block.set(f, offset + 1);
      offset += packetSize;
    }
  }
  blocks.push(block);

  const out = new Uint8Array(blocks.length * SD_BLE_SIZE);
  blocks.forEach((b, i) => out.set(b, i * SD_BLE_SIZE));
  return out;
}

test('parses a simple run of length-prefixed frames', () => {
  const frames = [frame(25), frame(30), frame(22)];
  const raw = new Uint8Array(frames.reduce((n, f) => n + f.length + 1, 0));
  let at = 0;
  for (const f of frames) {
    raw[at] = f.length;
    raw.set(f, at + 1);
    at += f.length + 1;
  }

  const { frames: parsed, skipped } = parseOpusFrames(raw);
  assert.equal(skipped, 0);
  assert.deepEqual(
    parsed.map((f) => Array.from(f)),
    frames.map((f) => Array.from(f)),
  );
});

test('recovers every frame across block boundaries and stale tails', () => {
  const frames = Array.from({ length: 500 }, (_, i) => frame(20 + (i % 30), i % 200));
  const raw = packLikeFirmware(frames);

  const { frames: parsed, skipped } = parseOpusFrames(raw);

  assert.equal(parsed.length, frames.length, 'no frame should be lost');
  assert.deepEqual(
    parsed.map((f) => Array.from(f)),
    frames.map((f) => Array.from(f)),
  );
  assert.ok(skipped > 0, 'stray boundary bytes and stale tails should be skipped');
  assert.ok(skipped / raw.length < 0.1, `skip ratio ${skipped / raw.length} should stay small`);
});

test('recovers after a shifted block grid', () => {
  const frames = Array.from({ length: 200 }, (_, i) => frame(24, i % 100));
  const aligned = packLikeFirmware(frames);
  // A short write anywhere in the file's history offsets everything after it.
  const shifted = new Uint8Array(aligned.length + 3);
  shifted.set([0x00, 0x11, 0x22], 0);
  shifted.set(aligned, 3);

  const { frames: parsed } = parseOpusFrames(shifted);
  assert.equal(parsed.length, frames.length);
});

test('rejects candidates whose length byte is not backed by a TOC', () => {
  const raw = new Uint8Array([25, 0x00, 0x01, 0x02, 3, OPUS_TOC, 0xaa, 0xbb]);
  const { frames: parsed } = parseOpusFrames(raw);
  assert.equal(parsed.length, 1);
  assert.deepEqual(Array.from(parsed[0]), [OPUS_TOC, 0xaa, 0xbb]);
});

test('ignores a truncated trailing frame', () => {
  const raw = new Uint8Array([30, OPUS_TOC, 0x01, 0x02]);
  const { frames: parsed } = parseOpusFrames(raw);
  assert.equal(parsed.length, 0);
});

test('handles empty input', () => {
  const { frames: parsed, skipped } = parseOpusFrames(new Uint8Array(0));
  assert.equal(parsed.length, 0);
  assert.equal(skipped, 0);
});

test('duration follows the fixed 10 ms frame size', () => {
  assert.equal(framesToSeconds(100), 1);
  assert.equal(framesToSeconds(6000), 60);
});

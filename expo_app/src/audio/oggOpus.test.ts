import assert from 'node:assert/strict';
import test from 'node:test';

import { OPUS_TOC } from '../ble/constants';
import { muxOggOpus, oggCrc32 } from './oggOpus';

function frame(length = 25, fill = 0x42): Uint8Array {
  const f = new Uint8Array(length);
  f[0] = OPUS_TOC;
  f.fill(fill, 1);
  return f;
}

function readU32LE(bytes: Uint8Array, at: number): number {
  return (bytes[at] | (bytes[at + 1] << 8) | (bytes[at + 2] << 16) | (bytes[at + 3] << 24)) >>> 0;
}

interface Page {
  headerType: number;
  granulePosition: number;
  sequence: number;
  segments: number[];
  payload: Uint8Array;
  totalLength: number;
  crcValid: boolean;
}

function readPage(bytes: Uint8Array, at: number): Page {
  assert.deepEqual(Array.from(bytes.subarray(at, at + 4)), [0x4f, 0x67, 0x67, 0x53], 'OggS');
  assert.equal(bytes[at + 4], 0, 'stream structure version');

  const segmentCount = bytes[at + 26];
  const segments = Array.from(bytes.subarray(at + 27, at + 27 + segmentCount));
  const payloadLength = segments.reduce((sum, s) => sum + s, 0);
  const totalLength = 27 + segmentCount + payloadLength;

  const stored = readU32LE(bytes, at + 22);
  const page = bytes.slice(at, at + totalLength);
  new DataView(page.buffer).setUint32(22, 0, true);

  return {
    headerType: bytes[at + 5],
    granulePosition: readU32LE(bytes, at + 6) + readU32LE(bytes, at + 10) * 0x100000000,
    sequence: readU32LE(bytes, at + 18),
    segments,
    payload: bytes.subarray(at + 27 + segmentCount, at + totalLength),
    totalLength,
    crcValid: oggCrc32(page) === stored,
  };
}

function readAllPages(bytes: Uint8Array): Page[] {
  const pages: Page[] = [];
  let at = 0;
  while (at < bytes.length) {
    const page = readPage(bytes, at);
    pages.push(page);
    at += page.totalLength;
  }
  return pages;
}

function ascii(bytes: Uint8Array): string {
  return String.fromCharCode(...bytes);
}

/**
 * The first page of a real Ogg Opus file, so the CRC is checked against an
 * external encoder rather than against our own arithmetic. Produced with:
 *
 *   ffmpeg -f lavfi -i "sine=frequency=440:duration=2:sample_rate=16000" \
 *     -c:a libopus -b:a 20k -frame_duration 10 -application lowdelay -ac 1 ffref.opus
 *
 * Its stored CRC is 0x3700ae85 at bytes 22..25.
 */
const FFMPEG_FIRST_PAGE = new Uint8Array([
  0x4f, 0x67, 0x67, 0x53, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x8a,
  0x85, 0x05, 0x00, 0x00, 0x00, 0x00, 0x85, 0xae, 0x00, 0x37, 0x01, 0x13, 0x4f, 0x70, 0x75, 0x73,
  0x48, 0x65, 0x61, 0x64, 0x01, 0x01, 0x78, 0x00, 0x80, 0x3e, 0x00, 0x00, 0x00, 0x00, 0x00,
]);

test('reproduces the page CRC that ffmpeg wrote', () => {
  const zeroed = FFMPEG_FIRST_PAGE.slice();
  new DataView(zeroed.buffer).setUint32(22, 0, true);
  assert.equal(oggCrc32(zeroed), 0x3700ae85);
});

test('reads the ffmpeg page back through our own page reader', () => {
  const page = readPage(FFMPEG_FIRST_PAGE, 0);
  assert.ok(page.crcValid);
  assert.equal(page.headerType, 0x02);
  assert.deepEqual(page.segments, [19]);
  assert.equal(ascii(page.payload.subarray(0, 8)), 'OpusHead');
  assert.equal(readU32LE(page.payload, 12), 16000, 'ffmpeg reports the 16 kHz input rate too');
});

test('produces nothing for zero frames', () => {
  assert.equal(muxOggOpus([]).length, 0);
});

test('emits OpusHead and OpusTags header pages then audio pages', () => {
  const frames = Array.from({ length: 5 }, (_, i) => frame(25, i));
  const pages = readAllPages(muxOggOpus(frames));

  assert.equal(pages.length, 3);

  const [head, tags, audio] = pages;

  assert.equal(head.headerType, 0x02, 'first page carries the BOS flag');
  assert.equal(ascii(head.payload.subarray(0, 8)), 'OpusHead');
  assert.equal(head.payload[8], 1, 'OpusHead version');
  assert.equal(head.payload[9], 1, 'mono');
  assert.equal(head.payload[10] | (head.payload[11] << 8), 0, 'pre-skip');
  assert.equal(readU32LE(head.payload, 12), 16000, 'input sample rate');
  assert.equal(head.payload[18], 0, 'channel mapping family 0');
  assert.equal(head.granulePosition, 0);

  assert.equal(tags.headerType, 0);
  assert.equal(ascii(tags.payload.subarray(0, 8)), 'OpusTags');
  assert.equal(tags.granulePosition, 0);

  assert.equal(audio.headerType, 0x04, 'last page carries the EOS flag');
  assert.deepEqual(audio.segments, [25, 25, 25, 25, 25]);
  assert.equal(audio.granulePosition, 5 * 480, 'granule counts 48 kHz samples');

  pages.forEach((page, i) => assert.ok(page.crcValid, `page ${i} CRC`));
  pages.forEach((page, i) => assert.equal(page.sequence, i, `page ${i} sequence`));
});

test('splits audio across pages and only flags EOS on the last', () => {
  const frames = Array.from({ length: 250 }, () => frame(20));
  const pages = readAllPages(muxOggOpus(frames, { framesPerPage: 100 }));

  const audioPages = pages.slice(2);
  assert.equal(audioPages.length, 3);
  assert.deepEqual(
    audioPages.map((p) => p.segments.length),
    [100, 100, 50],
  );
  assert.deepEqual(
    audioPages.map((p) => p.granulePosition),
    [100 * 480, 200 * 480, 250 * 480],
  );
  assert.deepEqual(
    audioPages.map((p) => p.headerType),
    [0, 0, 0x04],
  );
  pages.forEach((page, i) => assert.ok(page.crcValid, `page ${i} CRC`));
});

test('a frame longer than 255 bytes laces across multiple segments', () => {
  const long = frame(300);
  const pages = readAllPages(muxOggOpus([long], { framesPerPage: 1 }));
  assert.deepEqual(pages[2].segments, [255, 45]);
  assert.equal(pages[2].payload.length, 300);
});

test('rejects a page size the segment table cannot express', () => {
  assert.throws(() => muxOggOpus([frame()], { framesPerPage: 256 }), /framesPerPage/);
});

test('is deterministic for the same frames', () => {
  const frames = Array.from({ length: 40 }, (_, i) => frame(25, i));
  assert.deepEqual(Array.from(muxOggOpus(frames)), Array.from(muxOggOpus(frames)));
});

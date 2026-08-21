/**
 * Wraps bare Opus frames in an Ogg container (RFC 7845).
 *
 * The device stores raw Opus packets with no container. react-native-audio-api
 * decodes ".opus" through libopusfile, which only reads Ogg, so muxing here is
 * what makes the recordings playable without a WASM build or a custom native
 * module.
 */

import { OPUS_SAMPLE_RATE } from '../ble/constants';

const OGG_CAPTURE = [0x4f, 0x67, 0x67, 0x53]; // "OggS"

const HEADER_TYPE_BOS = 0x02;
const HEADER_TYPE_EOS = 0x04;

const MAX_SEGMENTS_PER_PAGE = 255;

/**
 * Opus always reports timing at 48 kHz regardless of the encoder's input rate,
 * so one 10 ms frame advances the granule position by 480.
 */
const GRANULE_PER_FRAME = 480;

/** Ogg uses a non-reflected CRC-32 with polynomial 0x04c11db7, zero init, no final xor. */
const CRC_TABLE = (() => {
  const table = new Uint32Array(256);
  for (let i = 0; i < 256; i += 1) {
    let r = i << 24;
    for (let bit = 0; bit < 8; bit += 1) {
      r = (r & 0x80000000) !== 0 ? ((r << 1) ^ 0x04c11db7) >>> 0 : (r << 1) >>> 0;
    }
    table[i] = r >>> 0;
  }
  return table;
})();

export function oggCrc32(bytes: Uint8Array): number {
  let crc = 0;
  for (let i = 0; i < bytes.length; i += 1) {
    crc = ((crc << 8) ^ CRC_TABLE[((crc >>> 24) ^ bytes[i]) & 0xff]) >>> 0;
  }
  return crc >>> 0;
}

/** Ogg lacing: floor(L/255) bytes of 255, then L%255. A 255-multiple needs the trailing 0. */
function lacingValues(packetLength: number): number[] {
  const values: number[] = [];
  let remaining = packetLength;
  while (remaining >= 255) {
    values.push(255);
    remaining -= 255;
  }
  values.push(remaining);
  return values;
}

interface PageParams {
  headerType: number;
  granulePosition: number;
  serial: number;
  pageSequence: number;
  packets: Uint8Array[];
}

function buildPage({
  headerType,
  granulePosition,
  serial,
  pageSequence,
  packets,
}: PageParams): Uint8Array {
  const segments: number[] = [];
  for (const packet of packets) {
    segments.push(...lacingValues(packet.length));
  }
  if (segments.length > MAX_SEGMENTS_PER_PAGE) {
    throw new Error(`ogg page would need ${segments.length} segments, max is ${MAX_SEGMENTS_PER_PAGE}`);
  }

  const payloadLength = packets.reduce((sum, packet) => sum + packet.length, 0);
  const page = new Uint8Array(27 + segments.length + payloadLength);
  const view = new DataView(page.buffer);

  page.set(OGG_CAPTURE, 0);
  page[4] = 0; // stream structure version
  page[5] = headerType;

  // 64-bit little-endian granule position. Values stay well inside 2^53 here
  // (2^53 granules is over 5000 years of audio), so splitting into two 32-bit
  // halves is safe and avoids requiring BigInt.
  view.setUint32(6, granulePosition >>> 0, true);
  view.setUint32(10, Math.floor(granulePosition / 0x100000000), true);

  view.setUint32(14, serial >>> 0, true);
  view.setUint32(18, pageSequence >>> 0, true);
  view.setUint32(22, 0, true); // CRC placeholder, filled in below
  page[26] = segments.length;
  page.set(segments, 27);

  let at = 27 + segments.length;
  for (const packet of packets) {
    page.set(packet, at);
    at += packet.length;
  }

  view.setUint32(22, oggCrc32(page), true);
  return page;
}

function buildOpusHead(channels: number, preSkip: number, inputSampleRate: number): Uint8Array {
  const head = new Uint8Array(19);
  const view = new DataView(head.buffer);
  head.set([0x4f, 0x70, 0x75, 0x73, 0x48, 0x65, 0x61, 0x64], 0); // "OpusHead"
  head[8] = 1; // version
  head[9] = channels;
  view.setUint16(10, preSkip, true);
  view.setUint32(12, inputSampleRate, true);
  view.setInt16(16, 0, true); // output gain
  head[18] = 0; // channel mapping family 0
  return head;
}

function buildOpusTags(vendor: string): Uint8Array {
  const vendorBytes = utf8Encode(vendor);
  const tags = new Uint8Array(8 + 4 + vendorBytes.length + 4);
  const view = new DataView(tags.buffer);
  tags.set([0x4f, 0x70, 0x75, 0x73, 0x54, 0x61, 0x67, 0x73], 0); // "OpusTags"
  view.setUint32(8, vendorBytes.length, true);
  tags.set(vendorBytes, 12);
  view.setUint32(12 + vendorBytes.length, 0, true); // zero user comments
  return tags;
}

/** TextEncoder is not guaranteed on every RN runtime, and the vendor string is ASCII. */
function utf8Encode(value: string): Uint8Array {
  const bytes = new Uint8Array(value.length);
  for (let i = 0; i < value.length; i += 1) {
    bytes[i] = value.charCodeAt(i) & 0x7f;
  }
  return bytes;
}

export interface MuxOptions {
  channels?: number;
  /** Reported in OpusHead as the original input rate; decoding still happens at 48 kHz. */
  inputSampleRate?: number;
  /**
   * Samples the decoder should discard. The firmware does not pre-pad, and the
   * reference Python client decodes every frame, so zero keeps us byte-faithful.
   */
  preSkip?: number;
  /** Fixed by default so muxing the same frames twice produces identical bytes. */
  serial?: number;
  framesPerPage?: number;
  vendor?: string;
}

/**
 * Builds a complete Ogg Opus stream. Returns an empty buffer for zero frames,
 * since a header-only stream is not something any player handles gracefully.
 */
export function muxOggOpus(frames: Uint8Array[], options: MuxOptions = {}): Uint8Array {
  const {
    channels = 1,
    inputSampleRate = OPUS_SAMPLE_RATE,
    preSkip = 0,
    serial = 0x4f4d4931, // "OMI1"
    framesPerPage = 200,
    vendor = 'omi-expo-sync',
  } = options;

  if (frames.length === 0) {
    return new Uint8Array(0);
  }
  if (framesPerPage < 1 || framesPerPage > MAX_SEGMENTS_PER_PAGE) {
    throw new Error(`framesPerPage must be between 1 and ${MAX_SEGMENTS_PER_PAGE}`);
  }

  const pages: Uint8Array[] = [];
  let pageSequence = 0;

  pages.push(
    buildPage({
      headerType: HEADER_TYPE_BOS,
      granulePosition: 0,
      serial,
      pageSequence: pageSequence++,
      packets: [buildOpusHead(channels, preSkip, inputSampleRate)],
    }),
  );

  pages.push(
    buildPage({
      headerType: 0,
      granulePosition: 0,
      serial,
      pageSequence: pageSequence++,
      packets: [buildOpusTags(vendor)],
    }),
  );

  for (let start = 0; start < frames.length; start += framesPerPage) {
    const pagePackets = frames.slice(start, start + framesPerPage);
    const framesThroughPage = start + pagePackets.length;
    const isLast = framesThroughPage === frames.length;

    pages.push(
      buildPage({
        headerType: isLast ? HEADER_TYPE_EOS : 0,
        granulePosition: framesThroughPage * GRANULE_PER_FRAME + preSkip,
        serial,
        pageSequence: pageSequence++,
        packets: pagePackets,
      }),
    );
  }

  const total = pages.reduce((sum, page) => sum + page.length, 0);
  const out = new Uint8Array(total);
  let at = 0;
  for (const page of pages) {
    out.set(page, at);
    at += page.length;
  }
  return out;
}
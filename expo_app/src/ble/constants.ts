/**
 * Wire constants for the Omi DevKit v2 storage protocol.
 *
 * Source of truth: omi/firmware/devkit/src/storage.c and src/transport.c.
 * The reference client is omi/firmware/scripts/devkit/sd_sync/omi_sd.py.
 */

/** Advertised by the device. The storage service is NOT advertised. */
export const AUDIO_SERVICE_UUID = '19b10000-e8f2-537e-4f6c-d104768a1214';

export const STORAGE_SERVICE_UUID = '30295780-4301-eabd-2904-2849adfeae43';

/** Commands are written here, and every response and data block notifies back on it. */
export const STORAGE_CMD_CHAR_UUID = '30295781-4301-eabd-2904-2849adfeae43';

/** Read-only, little-endian, and it grows: the DevKit sends 50 bytes today. See parseRingInfo. */
export const STORAGE_INFO_CHAR_UUID = '30295782-4301-eabd-2904-2849adfeae43';

export const TIME_SYNC_SERVICE_UUID = '19b10030-e8f2-537e-4f6c-d104768a1214';

/** Write exactly 4 bytes, UTC epoch seconds, little-endian. */
export const TIME_SYNC_WRITE_CHAR_UUID = '19b10031-e8f2-537e-4f6c-d104768a1214';

/** Size of one storage data block. Offsets must be a multiple of this. */
export const SD_BLE_SIZE = 440;

/**
 * Shortest payload that still carries the ring fields, not the payload's actual length —
 * the firmware appends diagnostics past this and will keep doing so. Treat it as a minimum,
 * never as an equality check, or the next field added to the characteristic breaks parsing.
 */
export const STORAGE_INFO_BYTES = 21;

export const StorageCommand = {
  Read: 0,
  Delete: 1,
  Nuke: 2,
  Stop: 3,
  Heartbeat: 50,
} as const;

/** OR this into a segment number to read its timestamp index instead of its audio. */
export const SEGMENT_INDEX_FLAG = 0x80;

/**
 * Single-byte notification values. Any notification longer than one byte is raw
 * file data -- that length check is the entire demultiplexing rule.
 */
export const StorageStatus = {
  Ok: 0,
  InvalidSegment: 3,
  EmptySegment: 4,
  OffsetPastEnd: 5,
  InvalidCommand: 6,
  EndOfTransfer: 100,
  Deleted: 200,
} as const;

export function describeStatus(status: number): string {
  switch (status) {
    case StorageStatus.Ok:
      return 'accepted';
    case StorageStatus.InvalidSegment:
      return 'no such segment on the device';
    case StorageStatus.EmptySegment:
      return 'segment is empty';
    case StorageStatus.OffsetPastEnd:
      return 'requested offset is past the end of the segment';
    case StorageStatus.InvalidCommand:
      return 'device rejected the command';
    case StorageStatus.EndOfTransfer:
      return 'end of transfer';
    case StorageStatus.Deleted:
      return 'segment deleted';
    default:
      return `unknown status ${status}`;
  }
}

/** Opus encoder settings are fixed in firmware, so every payload starts with this TOC byte. */
export const OPUS_TOC = 0xb0;
export const OPUS_SAMPLE_RATE = 16000;
export const OPUS_FRAME_SAMPLES = 160;
export const OPUS_FRAMES_PER_SECOND = 100;

/** Measured on a DevKit v2 at CELT 20 kbps; see omi/firmware/devkit/DEBUGGING.md. */
export const RECORDING_BYTES_PER_SECOND = 2391;

/** Bytes per record in the .idx timestamp sidecar. */
export const INDEX_RECORD_SIZE = 16;

/**
 * The firmware sleeps 500 ms in the command ACK handler and 1000 ms in
 * setup_storage_tx, so roughly this long passes before the first data block.
 * Anything shorter than this is not a stall.
 */
export const TRANSFER_STARTUP_MS = 2500;

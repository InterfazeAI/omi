/**
 * The on-device implementation of `SyncStore`: segment files plus the manifest.
 */

import { appendToSegment, bytesOnDisk, writeIndexFile } from '../storage/recordings';
import { loadSegments, saveSegments } from './progressStore';
import type { SyncStore } from './syncEngine';

export const fileStore: SyncStore = {
  bytesOnDisk,
  appendToSegment: (seq, bytes) => {
    appendToSegment(seq, bytes);
  },
  writeIndexFile,
  loadSegments,
  saveSegments,
};

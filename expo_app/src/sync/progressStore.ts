/**
 * Reads and writes the sync manifest at recordings/manifest.json.
 *
 * The manifest's shape and its pure transformations live in ./segments; this
 * module is only the filesystem half.
 */

import { File } from 'expo-file-system';

import { bytesOnDisk, recordingsDirectory } from '../storage/recordings';
import { parseManifest, serializeManifest, type SegmentRecord } from './segments';

const MANIFEST_NAME = 'manifest.json';

function manifestFile(): File {
  return new File(recordingsDirectory(), MANIFEST_NAME);
}

export function loadSegments(): SegmentRecord[] {
  const file = manifestFile();
  if (!file.exists) {
    return [];
  }
  return parseManifest(file.textSync(), bytesOnDisk);
}

export function saveSegments(segments: SegmentRecord[]): void {
  const file = manifestFile();
  if (!file.exists) {
    file.create({ intermediates: true, overwrite: true });
  }
  file.write(serializeManifest(segments));
}

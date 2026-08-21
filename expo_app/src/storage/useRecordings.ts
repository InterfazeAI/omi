import { useCallback, useEffect, useMemo, useState } from 'react';

import { loadSegments, saveSegments } from '../sync/progressStore';
import type { SegmentRecord } from '../sync/segments';
import { chunksForSegment, deleteSegmentFiles, type RecordingChunk } from './recordings';

export interface RecordingItem extends RecordingChunk {
  /** True while the device may still append to the segment this chunk came from. */
  segmentIncomplete: boolean;
  evicted: boolean;
}

export interface RecordingsSummary {
  items: RecordingItem[];
  segments: SegmentRecord[];
  totalBytes: number;
  reload: () => void;
  clearAll: () => void;
}

export function useRecordings(): RecordingsSummary {
  const [segments, setSegments] = useState<SegmentRecord[]>([]);

  const reload = useCallback(() => {
    setSegments(loadSegments());
  }, []);

  useEffect(reload, [reload]);

  const clearAll = useCallback(() => {
    for (const segment of loadSegments()) {
      deleteSegmentFiles(segment.seq);
    }
    saveSegments([]);
    setSegments([]);
  }, []);

  // Memoised because the playback ticker re-renders the screen several times a
  // second, and rebuilding every chunk each time would hand the list new object
  // identities on every tick.
  const { items, totalBytes } = useMemo(() => {
    const built: RecordingItem[] = [];
    let bytes = 0;

    // Newest first: the most recent audio is what anyone opens the app to hear.
    for (const segment of [...segments].sort((a, b) => b.seq - a.seq)) {
      bytes += segment.bytesPulled;
      const chunks = chunksForSegment(segment.seq, segment.bytesPulled, segment.indexRecords);
      for (const chunk of [...chunks].reverse()) {
        built.push({
          ...chunk,
          segmentIncomplete: !segment.complete && !segment.evicted,
          evicted: segment.evicted,
        });
      }
    }

    return { items: built, totalBytes: bytes };
  }, [segments]);

  return { items, segments, totalBytes, reload, clearAll };
}

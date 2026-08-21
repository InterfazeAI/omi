import 'package:flutter_test/flutter_test.dart';

import 'package:omi/services/wals/ring_session_timeline.dart';

/// opusFS320: 320-sample frames at 16 kHz.
const int fps = 50;

/// Production uses sdcardChunkSizeSecs (180s) worth of frames per chunk. These
/// tests use 4 seconds so a chunk boundary is reachable in a readable number of
/// records; the timeline treats it as an opaque frame count either way.
const int chunkFrames = 4 * fps;

List<List<int>> frames(int count) => List.generate(count, (i) => [i & 0xFF]);

RingSessionTimeline makeTimeline() => RingSessionTimeline(framesPerSecond: fps, chunkFrames: chunkFrames);

List<TimelineChunk> drain(RingSessionTimeline timeline, {bool finalFlush = true}) {
  final out = <TimelineChunk>[];
  while (true) {
    final chunk = timeline.takeChunk(finalFlush: finalFlush);
    if (chunk == null) return out;
    out.add(chunk);
  }
}

void main() {
  group('RingSessionTimeline continuous audio', () {
    test('advances the clock by frame count when records are contiguous', () {
      final timeline = makeTimeline();
      timeline.anchor(1000);

      // 10 records of 50 frames = 10 seconds of continuous audio, each record's
      // timestamp exactly matching where the frame count puts it.
      for (int i = 0; i < 10; i++) {
        timeline.addRecord(timestamp: 1000 + i, frames: frames(fps));
      }

      final chunks = drain(timeline);
      expect(chunks.map((c) => c.timerStart), [1000, 1004, 1008]);
      expect(chunks.map((c) => c.frames.length), [chunkFrames, chunkFrames, fps * 2]);
    });

    test('tolerates timestamps drifting within the tolerance window', () {
      final timeline = makeTimeline();
      timeline.anchor(1000);

      // Records nominally 1s apart but reported 2s ahead — inside the 3s
      // tolerance, so this must not be read as a new session.
      for (int i = 0; i < 8; i++) {
        timeline.addRecord(timestamp: 1002 + i, frames: frames(fps));
      }

      final chunks = drain(timeline);
      expect(chunks.map((c) => c.timerStart), [1000, 1004]);
    });
  });

  group('RingSessionTimeline gapped audio', () {
    test('re-anchors a session that starts after a silence gap', () {
      final timeline = makeTimeline();
      timeline.anchor(1000);

      // Session one: 3 seconds of speech starting at t=1000.
      for (int i = 0; i < 3; i++) {
        timeline.addRecord(timestamp: 1000 + i, frames: frames(fps));
      }
      // Ten minutes of silence, then session two.
      for (int i = 0; i < 3; i++) {
        timeline.addRecord(timestamp: 1600 + i, frames: frames(fps));
      }

      final chunks = drain(timeline);

      // Session one's tail is cut short at the boundary rather than absorbing
      // session two's frames, and session two lands at its own wall-clock time
      // instead of at 1003 where frame counting would have put it.
      expect(chunks.length, 2);
      expect(chunks[0].timerStart, 1000);
      expect(chunks[0].frames.length, 3 * fps);
      expect(chunks[1].timerStart, 1600);
      expect(chunks[1].frames.length, 3 * fps);
    });

    test('keeps three separated bursts at their own timestamps', () {
      final timeline = makeTimeline();
      timeline.anchor(5000);

      for (final start in [5000, 5300, 5900]) {
        for (int i = 0; i < 2; i++) {
          timeline.addRecord(timestamp: start + i, frames: frames(fps));
        }
      }

      final chunks = drain(timeline);
      expect(chunks.map((c) => c.timerStart), [5000, 5300, 5900]);
      expect(chunks.map((c) => c.frames.length), [2 * fps, 2 * fps, 2 * fps]);
    });

    test('a gap mid-chunk splits the chunk at the session boundary', () {
      final timeline = makeTimeline();
      timeline.anchor(2000);

      // Only 1 second of speech, well under a full 4-second chunk...
      timeline.addRecord(timestamp: 2000, frames: frames(fps));
      // ...then a gap. The short first session must still be emitted on its own.
      timeline.addRecord(timestamp: 2500, frames: frames(fps));

      final streamed = drain(timeline, finalFlush: false);
      expect(streamed.length, 1);
      expect(streamed[0].timerStart, 2000);
      expect(streamed[0].frames.length, fps);

      final tail = drain(timeline);
      expect(tail.length, 1);
      expect(tail[0].timerStart, 2500);
    });

    test('a session longer than one chunk still re-anchors afterwards', () {
      final timeline = makeTimeline();
      timeline.anchor(100);

      // 10 seconds of speech spanning two full chunks plus a tail.
      for (int i = 0; i < 10; i++) {
        timeline.addRecord(timestamp: 100 + i, frames: frames(fps));
      }
      // Then a gap of five minutes.
      timeline.addRecord(timestamp: 410, frames: frames(fps));

      final chunks = drain(timeline);
      expect(chunks.map((c) => c.timerStart), [100, 104, 108, 410]);
      expect(chunks.map((c) => c.frames.length), [chunkFrames, chunkFrames, 2 * fps, fps]);
    });
  });

  group('RingSessionTimeline untrusted device clock', () {
    test('ignores record timestamps entirely when rtc_valid was 0', () {
      final timeline = makeTimeline();
      timeline.anchor(9000);

      // Same gapped stream as above, but the caller passes null because the
      // device reported its clock was never synced. Without a trustworthy clock
      // there is nothing better than continuous extrapolation.
      timeline.addRecord(timestamp: null, frames: frames(3 * fps));
      timeline.addRecord(timestamp: null, frames: frames(3 * fps));

      final chunks = drain(timeline);
      expect(chunks.map((c) => c.timerStart), [9000, 9004]);
    });
  });

  group('RingSessionTimeline streaming behaviour', () {
    test('holds back a partial chunk until the final flush', () {
      final timeline = makeTimeline();
      timeline.anchor(0);
      timeline.addRecord(timestamp: 0, frames: frames(fps));

      expect(timeline.hasChunkReady, isFalse);
      expect(timeline.takeChunk(finalFlush: false), isNull);
      expect(timeline.takeChunk(finalFlush: true)!.frames.length, fps);
    });

    test('anchors on the first record when none was set explicitly', () {
      final timeline = makeTimeline();
      timeline.addRecord(timestamp: 777, frames: frames(fps));
      expect(timeline.isAnchored, isTrue);
      expect(timeline.takeChunk(finalFlush: true)!.timerStart, 777);
    });
  });
}

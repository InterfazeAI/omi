/// Places ring-buffer records on a wall clock and cuts them into WAL-sized
/// chunks.
///
/// The ring protocol carries a timestamp on every record, but the naive
/// consumer anchors on the first one and then advances purely by frame count.
/// That is correct only while recording is continuous. Firmware that gates
/// recording on voice activity emits one contiguous run of records per speech
/// session and nothing in between, so frame count stops tracking elapsed time
/// and every session after the first lands early by the total length of the
/// silence before it — far outside the couple of minutes the backend allows when
/// deciding whether synced audio belongs to an existing conversation.
///
/// This class watches for that: when a record's own timestamp runs materially
/// ahead of where the frame count says it should be, the pending frames are cut
/// off at that point and the clock re-anchors to the record's timestamp. Frames
/// from two sessions therefore never share a chunk.
///
/// Deliberately free of I/O and BLE state so the timeline rules can be tested
/// on their own.
library;

/// A run of frames ready to be written, with the wall-clock second it starts at.
class TimelineChunk {
  final List<List<int>> frames;
  final int timerStart;

  const TimelineChunk({required this.frames, required this.timerStart});
}

class RingSessionTimeline {
  /// Opus frames per second for the negotiated codec (50 for opusFS320).
  final int framesPerSecond;

  /// Frames per emitted chunk while streaming.
  final int chunkFrames;

  /// How far a record's timestamp may run ahead of the frame-count
  /// extrapolation before it counts as a new session. Record timestamps have
  /// one-second resolution and the extrapolation truncates, so a few seconds of
  /// slack is expected even on genuinely continuous audio.
  final int gapToleranceSecs;

  RingSessionTimeline({
    required this.framesPerSecond,
    required this.chunkFrames,
    this.gapToleranceSecs = 3,
  });

  final List<List<int>> _frames = [];
  final List<_Boundary> _boundaries = [];

  bool _anchored = false;
  int _chunkTimerStart = 0;
  int _anchorTs = 0;
  int _framesSinceAnchor = 0;

  bool get isAnchored => _anchored;
  int get pendingFrames => _frames.length;

  /// True when [takeChunk] would return something without draining the tail.
  bool get hasChunkReady => _boundaries.isNotEmpty || _frames.length >= chunkFrames;

  /// Current write position on the wall clock. Exposed for logging and tests.
  int get timerStart => _chunkTimerStart;

  int get _fps => framesPerSecond <= 0 ? 1 : framesPerSecond;

  /// Start the timeline at [timerStart]. The caller decides where that comes
  /// from: the first record's own timestamp when the device clock is
  /// trustworthy, or `now - estimated duration` when it is not.
  void anchor(int timerStart) {
    _anchored = true;
    _chunkTimerStart = timerStart;
    _anchorTs = timerStart;
    _framesSinceAnchor = 0;
  }

  /// Append one record's frames.
  ///
  /// Pass [timestamp] as null when the device reports its clock is not synced —
  /// its timestamps are then meaningless and gap detection is skipped, leaving
  /// the frame-count extrapolation as the only available estimate.
  void addRecord({required int? timestamp, required List<List<int>> frames}) {
    if (!_anchored) {
      anchor(timestamp ?? 0);
    } else if (timestamp != null && timestamp > 0) {
      final expected = _anchorTs + _framesSinceAnchor ~/ _fps;
      if (timestamp - expected > gapToleranceSecs) {
        _boundaries.add(_Boundary(frameOffset: _frames.length, timerStart: timestamp));
        _anchorTs = timestamp;
        _framesSinceAnchor = 0;
      }
    }

    _frames.addAll(frames);
    _framesSinceAnchor += frames.length;
  }

  /// Pop the next chunk, or null when nothing is ready.
  ///
  /// While streaming, only full [chunkFrames] chunks come out, except that a
  /// session boundary always cuts short — holding a session's tail back would
  /// merge it with the next session. Pass [finalFlush] to drain the remainder.
  TimelineChunk? takeChunk({bool finalFlush = false}) {
    while (_boundaries.isNotEmpty && _boundaries.first.frameOffset <= 0) {
      _chunkTimerStart = _boundaries.removeAt(0).timerStart;
    }

    final int limit = _boundaries.isEmpty ? _frames.length : _boundaries.first.frameOffset;
    final int take;
    if (limit >= chunkFrames) {
      take = chunkFrames;
    } else if (limit > 0 && (finalFlush || _boundaries.isNotEmpty)) {
      take = limit;
    } else {
      return null;
    }

    final chunk = TimelineChunk(frames: _frames.sublist(0, take), timerStart: _chunkTimerStart);
    _frames.removeRange(0, take);
    for (final boundary in _boundaries) {
      boundary.frameOffset -= take;
    }
    _chunkTimerStart += take ~/ _fps;
    return chunk;
  }
}

class _Boundary {
  int frameOffset;
  final int timerStart;

  _Boundary({required this.frameOffset, required this.timerStart});
}

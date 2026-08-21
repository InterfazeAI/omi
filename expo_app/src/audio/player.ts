/**
 * Playback for a single recording chunk.
 *
 * The bytes on disk are the device's raw stream, so playing a chunk means:
 * read its byte range, resynchronise the Opus frames, wrap them in Ogg, and hand
 * the file to react-native-audio-api (which decodes Opus through libopusfile).
 * The decoded PCM is then driven by a plain AudioBufferSourceNode.
 *
 * A source node can only be started once, so pause/resume creates a new node
 * from the same decoded buffer and starts it at the saved offset.
 */

import { Directory, File, Paths } from 'expo-file-system';
import {
  AudioContext,
  AudioManager,
  type AudioBuffer,
  type AudioBufferSourceNode,
} from 'react-native-audio-api';

import { muxOggOpus } from './oggOpus';
import { framesToSeconds, parseOpusFrames } from './opusFrames';
import { readSegmentRange, type RecordingChunk } from '../storage/recordings';

const CACHE_DIR_NAME = 'chunk-audio';

export interface PlayerState {
  chunkId: string | null;
  loading: boolean;
  playing: boolean;
  positionSeconds: number;
  durationSeconds: number;
  error: string | null;
}

const IDLE_STATE: PlayerState = {
  chunkId: null,
  loading: false,
  playing: false,
  positionSeconds: 0,
  durationSeconds: 0,
  error: null,
};

function cacheDirectory(): Directory {
  const dir = new Directory(Paths.cache, CACHE_DIR_NAME);
  if (!dir.exists) {
    dir.create({ intermediates: true, idempotent: true });
  }
  return dir;
}

export interface DecodedChunk {
  frameCount: number;
  skippedBytes: number;
  seconds: number;
  uri: string;
}

/**
 * Turns a chunk's raw bytes into a playable .opus file in the cache directory.
 * Exposed separately from playback so the UI can report frame and skip counts,
 * which is the quickest way to tell a decode problem from a transfer problem.
 */
export function prepareChunk(chunk: RecordingChunk): DecodedChunk | null {
  const raw = readSegmentRange(chunk.seq, chunk.startByte, chunk.byteLength);
  if (raw.length === 0) {
    return null;
  }

  const { frames, skipped } = parseOpusFrames(raw);
  if (frames.length === 0) {
    return null;
  }

  const ogg = muxOggOpus(frames);
  const file = new File(cacheDirectory(), `chunk-${chunk.seq}-${chunk.index}.opus`);
  if (file.exists) {
    file.delete();
  }
  file.create({ intermediates: true, overwrite: true });
  file.write(ogg);

  return {
    frameCount: frames.length,
    skippedBytes: skipped,
    seconds: framesToSeconds(frames.length),
    uri: file.uri,
  };
}

export function clearChunkCache(): void {
  const dir = cacheDirectory();
  if (dir.exists) {
    dir.delete();
  }
}

type Listener = (state: PlayerState) => void;

export class ChunkPlayer {
  private context: AudioContext | null = null;

  private source: AudioBufferSourceNode | null = null;

  private buffer: AudioBuffer | null = null;

  private loadedChunkId: string | null = null;

  private startedAtContextTime = 0;

  private offsetAtStart = 0;

  private ticker: ReturnType<typeof setInterval> | null = null;

  private state: PlayerState = IDLE_STATE;

  private listeners = new Set<Listener>();

  subscribe(listener: Listener): () => void {
    this.listeners.add(listener);
    listener(this.state);
    return () => {
      this.listeners.delete(listener);
    };
  }

  getState(): PlayerState {
    return this.state;
  }

  private update(patch: Partial<PlayerState>): void {
    this.state = { ...this.state, ...patch };
    for (const listener of this.listeners) {
      listener(this.state);
    }
  }

  private ensureContext(): AudioContext {
    if (!this.context) {
      // Playback category so audio is not silenced by the ringer switch.
      AudioManager.setAudioSessionOptions({ iosCategory: 'playback' });
      this.context = new AudioContext();
    }
    return this.context;
  }

  /** Loads and starts a chunk. Restarts from the beginning if it is already loaded. */
  async play(chunk: RecordingChunk): Promise<void> {
    try {
      if (this.loadedChunkId !== chunk.id) {
        this.teardownSource();
        this.update({
          chunkId: chunk.id,
          loading: true,
          playing: false,
          positionSeconds: 0,
          durationSeconds: 0,
          error: null,
        });

        const prepared = prepareChunk(chunk);
        if (!prepared) {
          this.update({ loading: false, error: 'No audio frames could be recovered from this chunk' });
          return;
        }

        const context = this.ensureContext();
        this.buffer = await context.decodeAudioData(prepared.uri);
        this.loadedChunkId = chunk.id;
        this.update({ loading: false, durationSeconds: this.buffer.duration });
      }

      this.startFrom(0);
    } catch (error) {
      this.update({
        loading: false,
        playing: false,
        error: error instanceof Error ? error.message : String(error),
      });
    }
  }

  resume(): void {
    if (this.buffer && !this.state.playing) {
      this.startFrom(this.state.positionSeconds);
    }
  }

  pause(): void {
    if (!this.state.playing) {
      return;
    }
    const position = this.currentPosition();
    this.teardownSource();
    this.update({ playing: false, positionSeconds: position });
  }

  stop(): void {
    this.teardownSource();
    this.update({ playing: false, positionSeconds: 0 });
  }

  /** Releases the audio context. Call when the screen goes away. */
  async dispose(): Promise<void> {
    this.teardownSource();
    this.buffer = null;
    this.loadedChunkId = null;
    if (this.context) {
      const context = this.context;
      this.context = null;
      try {
        await context.close();
      } catch {
        // Already closed.
      }
    }
    this.update(IDLE_STATE);
  }

  private startFrom(offsetSeconds: number): void {
    if (!this.buffer) {
      return;
    }
    const context = this.ensureContext();
    this.teardownSource();

    const source = context.createBufferSource();
    source.buffer = this.buffer;
    source.connect(context.destination);
    source.onEnded = () => {
      this.teardownSource();
      this.update({ playing: false, positionSeconds: 0 });
    };

    // A freshly created context reports "suspended" until something schedules
    // work on it. Resuming explicitly keeps currentTime, which drives the
    // position readout, honest.
    if (context.state === 'suspended') {
      void context.resume();
    }

    const clamped = Math.max(0, Math.min(offsetSeconds, this.buffer.duration - 0.01));
    this.offsetAtStart = clamped;
    this.startedAtContextTime = context.currentTime;
    source.start(0, clamped);
    this.source = source;

    this.update({ playing: true, positionSeconds: clamped, error: null });

    this.ticker = setInterval(() => {
      if (this.state.playing) {
        this.update({ positionSeconds: this.currentPosition() });
      }
    }, 250);
  }

  private currentPosition(): number {
    if (!this.context || !this.buffer) {
      return this.state.positionSeconds;
    }
    const elapsed = this.context.currentTime - this.startedAtContextTime;
    return Math.min(this.offsetAtStart + Math.max(0, elapsed), this.buffer.duration);
  }

  private teardownSource(): void {
    if (this.ticker) {
      clearInterval(this.ticker);
      this.ticker = null;
    }
    if (this.source) {
      const source = this.source;
      this.source = null;
      // Clearing onEnded first stops the natural-end handler from firing on a
      // deliberate stop, which would reset the saved pause position to zero.
      source.onEnded = null;
      try {
        source.stop();
      } catch {
        // Never started, or already stopped.
      }
      try {
        source.disconnect();
      } catch {
        // Node already detached.
      }
    }
  }
}

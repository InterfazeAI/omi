import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { Alert, Pressable, ScrollView, StyleSheet, Text, View } from 'react-native';
import { useSafeAreaInsets } from 'react-native-safe-area-context';

import { REQUIRED_MTU } from '../src/ble/omiDevice';
import { describeSecurityError } from '../src/ble/pairing';
import { totalRingBytes } from '../src/ble/protocol';
import { useOmiConnection } from '../src/ble/useOmiConnection';
import { ChunkPlayer, type PlayerState } from '../src/audio/player';
import { bytesOnDisk } from '../src/storage/recordings';
import { useRecordings, type RecordingItem } from '../src/storage/useRecordings';
import { fileStore } from '../src/sync/fileStore';
import { SyncEngine, type SyncProgress } from '../src/sync/syncEngine';
import { Button, Card, Hint, ProgressBar, Row, StatusDot } from '../src/ui/components';
import { bytesToSeconds, formatBytes, formatClock, formatDuration } from '../src/ui/format';
import { colors, radius, spacing } from '../src/ui/theme';

const IDLE_PROGRESS: SyncProgress = {
  phase: 'idle',
  message: '',
  currentSeq: null,
  segmentsDone: 0,
  segmentsTotal: 0,
  bytesPulled: 0,
  bytesTarget: 0,
  kbps: 0,
};

export default function HomeScreen() {
  const insets = useSafeAreaInsets();
  const connection = useOmiConnection();
  const recordings = useRecordings();

  const [progress, setProgress] = useState<SyncProgress>(IDLE_PROGRESS);
  const [playerState, setPlayerState] = useState<PlayerState | null>(null);

  const engineRef = useRef<SyncEngine | null>(null);
  const playerRef = useRef<ChunkPlayer | null>(null);

  if (!playerRef.current) {
    playerRef.current = new ChunkPlayer();
  }

  useEffect(() => {
    const player = playerRef.current!;
    const unsubscribe = player.subscribe(setPlayerState);
    return () => {
      unsubscribe();
      player.dispose().catch(() => {});
    };
  }, []);

  const syncing = progress.phase === 'reading-info' || progress.phase === 'waiting' || progress.phase === 'pulling';

  // Let a disconnect stop the transfer first, so the device closes its read
  // handle and persists its offset instead of being cut off mid-block.
  const { setBeforeDisconnect } = connection;
  useEffect(() => {
    setBeforeDisconnect(async () => {
      const engine = engineRef.current;
      if (!engine?.isRunning) {
        return;
      }
      engine.cancel();
      await waitFor(() => !engine.isRunning, 3000);
    });
    return () => setBeforeDisconnect(null);
  }, [setBeforeDisconnect]);

  const { client, refreshInfo, releaseBond } = connection;
  const { reload: reloadRecordings } = recordings;

  const startSync = useCallback(async () => {
    if (!client || syncing) {
      return;
    }

    const engine = new SyncEngine(client, fileStore);
    engineRef.current = engine;
    setProgress({ ...IDLE_PROGRESS, phase: 'reading-info', message: 'Starting' });

    const result = await engine.run(setProgress);

    engineRef.current = null;
    reloadRecordings();
    await refreshInfo();

    if (result.error) {
      Alert.alert('Sync stopped', result.error);
    }
  }, [client, refreshInfo, reloadRecordings, syncing]);

  const cancelSync = useCallback(() => {
    engineRef.current?.cancel();
  }, []);

  const togglePlayback = useCallback(
    async (item: RecordingItem) => {
      const player = playerRef.current!;
      const state = player.getState();
      if (state.chunkId === item.id) {
        if (state.playing) {
          player.pause();
        } else if (state.positionSeconds > 0) {
          player.resume();
        } else {
          await player.play(item);
        }
        return;
      }
      await player.play(item);
    },
    [],
  );

  const confirmClear = useCallback(() => {
    Alert.alert(
      'Delete downloaded audio?',
      'Recordings on the phone are removed. Nothing on the device is touched, so you can sync them again.',
      [
        { text: 'Cancel', style: 'cancel' },
        {
          text: 'Delete',
          style: 'destructive',
          onPress: () => {
            playerRef.current?.stop();
            recordings.clearAll();
          },
        },
      ],
    );
  }, [recordings]);

  // Two prompts rather than one: this erases the card, and the firmware wipes before it unpairs
  // precisely so a new owner cannot inherit the audio. A single tap next to Disconnect is not
  // enough friction for something no amount of re-pairing undoes.
  const confirmRelease = useCallback(() => {
    Alert.alert(
      'Hand this device to someone else?',
      'The DevKit erases every recording on its card before it gives up the pairing, so a new owner cannot read your audio. Sync anything you want to keep first.',
      [
        { text: 'Cancel', style: 'cancel' },
        {
          text: 'Continue',
          style: 'destructive',
          onPress: () =>
            Alert.alert(
              'Erase the card and unpair?',
              'This cannot be undone. Downloads already on this phone are kept.',
              [
                { text: 'Cancel', style: 'cancel' },
                {
                  text: 'Erase and unpair',
                  style: 'destructive',
                  onPress: () => {
                    void releaseBond();
                  },
                },
              ],
            ),
        },
      ],
    );
  }, [releaseBond]);

  const deviceSummary = useMemo(() => {
    const info = connection.info;
    if (!info) {
      return null;
    }
    const total = totalRingBytes(info);
    let pending = 0;
    for (let seq = info.oldestSeq; seq <= info.newestSeq; seq += 1) {
      const segmentTotal = seq === info.newestSeq ? info.newestBytes : info.segmentBytes;
      pending += Math.max(0, segmentTotal - bytesOnDisk(seq));
    }
    return { total, pending, segments: info.count };
  }, [connection.info, recordings.segments]);

  const progressRatio = progress.bytesTarget > 0 ? progress.bytesPulled / progress.bytesTarget : 0;

  return (
    <ScrollView
      style={styles.screen}
      contentContainerStyle={[
        styles.content,
        { paddingTop: insets.top + spacing.lg, paddingBottom: insets.bottom + spacing.xl },
      ]}
    >
      <Text style={styles.heading}>Omi Sync</Text>
      <Text style={styles.subheading}>Pull recordings off a DevKit v2 SD card</Text>

      <Card title="Device">
        <DeviceStatus connection={connection} />

        <View style={styles.buttonRow}>
          {connection.status === 'connected' || connection.status === 'disconnecting' ? (
            <Button
              label="Disconnect"
              variant="secondary"
              onPress={() => {
                void connection.disconnect();
              }}
              busy={connection.status === 'disconnecting'}
              style={styles.grow}
            />
          ) : (
            <Button
              label={connectLabel(connection.status)}
              onPress={() => {
                void connection.connect();
              }}
              disabled={!connection.canConnect}
              busy={connection.status === 'connecting' || connection.status === 'pairing'}
              style={styles.grow}
            />
          )}
        </View>

        {connection.error ? <Hint tone="danger">{connection.error}</Hint> : null}

        <PairingHint connection={connection} />

        {connection.pairing?.linkEncrypted && connection.status === 'connected' ? (
          <Pressable onPress={confirmRelease} accessibilityRole="button">
            <Text style={styles.releaseLabel}>Unpair and hand device over</Text>
          </Pressable>
        ) : null}

        {connection.status === 'bluetooth-off' ? (
          <Hint tone="warning">Bluetooth is off. Turn it on to find your device.</Hint>
        ) : null}

        {connection.status === 'unauthorised' ? (
          <Hint tone="warning">
            Allow Bluetooth for this app in Settings, then reopen it.
          </Hint>
        ) : null}

        {connection.status === 'unsupported' ? (
          <Hint tone="warning">
            This device has no Bluetooth LE radio. The iOS Simulator cannot do BLE, so syncing needs
            a real iPhone.
          </Hint>
        ) : null}

        {connection.status === 'scanning' ? (
          <Hint>
            Scanning. The DevKit only advertises once its SD card mounts, so if it never appears,
            check the card before the radio.
          </Hint>
        ) : null}

        {connection.mtu !== null && connection.mtu > 0 && connection.mtu < REQUIRED_MTU ? (
          <Hint tone="warning">
            Negotiated MTU is {connection.mtu}, below the {REQUIRED_MTU} needed for 440-byte blocks.
            Transfers will stall.
          </Hint>
        ) : null}
      </Card>

      {connection.status === 'connected' && deviceSummary ? (
        <Card title="Storage">
          <Row label="Segments on card" value={String(deviceSummary.segments)} />
          <Row label="Recorded" value={formatBytes(deviceSummary.total)} />
          <Row
            label="Estimated length"
            value={formatDuration(bytesToSeconds(deviceSummary.total))}
          />
          <Row
            label="Not yet downloaded"
            value={
              deviceSummary.pending > 0
                ? `${formatBytes(deviceSummary.pending)} (${formatDuration(bytesToSeconds(deviceSummary.pending))})`
                : 'Nothing'
            }
            tone={deviceSummary.pending > 0 ? undefined : 'muted'}
          />
          {connection.clockSynced ? null : (
            <Hint>
              The device clock could not be set, so new recordings may lack wall-clock timestamps.
            </Hint>
          )}

          <View style={styles.buttonRow}>
            {syncing ? (
              <Button label="Stop" variant="secondary" onPress={cancelSync} style={styles.grow} />
            ) : (
              <Button
                label={deviceSummary.pending > 0 ? 'Sync new audio' : 'Check again'}
                onPress={() => {
                  void startSync();
                }}
                style={styles.grow}
              />
            )}
          </View>

          {progress.phase !== 'idle' ? (
            <View style={styles.progressBlock}>
              <ProgressBar value={progressRatio} />
              <View style={styles.progressMeta}>
                <Text style={styles.progressText}>{progress.message}</Text>
                <Text style={styles.progressText}>
                  {syncing && progress.kbps > 0 ? `${progress.kbps.toFixed(1)} KB/s` : ''}
                </Text>
              </View>
              {progress.bytesTarget > 0 ? (
                <Text style={styles.progressSub}>
                  {formatBytes(progress.bytesPulled)} of {formatBytes(progress.bytesTarget)}
                  {progress.segmentsTotal > 1
                    ? ` · segment ${Math.min(progress.segmentsDone + 1, progress.segmentsTotal)} of ${progress.segmentsTotal}`
                    : ''}
                </Text>
              ) : null}
            </View>
          ) : null}
        </Card>
      ) : null}

      <Card
        title={`Recordings${recordings.items.length > 0 ? ` (${recordings.items.length})` : ''}`}
        action={
          recordings.items.length > 0 ? (
            <Pressable onPress={confirmClear} accessibilityRole="button">
              <Text style={styles.clearLabel}>Clear</Text>
            </Pressable>
          ) : null
        }
      >
        {recordings.items.length === 0 ? (
          <Text style={styles.empty}>
            Nothing downloaded yet. Connect your DevKit and tap Sync.
          </Text>
        ) : (
          <>
            <Row label="On this phone" value={formatBytes(recordings.totalBytes)} />
            <View style={styles.list}>
              {recordings.items.map((item) => (
                <RecordingRow
                  key={item.id}
                  item={item}
                  playerState={playerState}
                  onToggle={() => {
                    void togglePlayback(item);
                  }}
                />
              ))}
            </View>
          </>
        )}
      </Card>
    </ScrollView>
  );
}

type Status = ReturnType<typeof useOmiConnection>['status'];

function idleStatusLabel(status: Status): string {
  switch (status) {
    case 'bluetooth-off':
      return 'Bluetooth off';
    case 'unauthorised':
      return 'Bluetooth not permitted';
    case 'unsupported':
      return 'No Bluetooth LE radio';
    case 'pairing':
      return 'Waiting for pairing';
    case 'connecting':
      return 'Connecting';
    case 'disconnecting':
      return 'Disconnecting';
    default:
      return 'Out of range';
  }
}

function connectLabel(status: Status): string {
  switch (status) {
    case 'available':
      return 'Connect';
    case 'pairing':
      return 'Pairing';
    case 'bluetooth-off':
      return 'Bluetooth off';
    case 'unauthorised':
      return 'Bluetooth not allowed';
    case 'unsupported':
      return 'Bluetooth unavailable';
    default:
      return 'Waiting for device';
  }
}

/**
 * The device knows why pairing failed and says so; this only has to route that to the right
 * remedy. Getting it wrong is worse than saying nothing, because a stale bond is cleared in iOS
 * Settings and a taken slot can only be cleared on the device -- following the wrong one just
 * wastes the user's time and leaves them no better informed.
 */
function PairingHint({ connection }: { connection: ReturnType<typeof useOmiConnection> }) {
  const { status, pairing, pairingVerdict } = connection;

  if (status === 'pairing') {
    return (
      <Hint>
        Accept the pairing request on this iPhone. The DevKit keeps its recordings encrypted and
        will not open them to an unpaired phone.
      </Hint>
    );
  }

  if (!pairingVerdict || pairingVerdict === 'ready' || pairingVerdict === 'not-required') {
    return null;
  }

  if (pairingVerdict === 'stale-bond') {
    return (
      <Hint tone="warning">
        This iPhone is still holding a pairing the device has thrown away, so it cannot connect.
        Open Settings, Bluetooth, tap the info button next to{' '}
        {connection.device?.name ?? 'the DevKit'} and choose Forget This Device, then connect
        again.
      </Hint>
    );
  }

  if (pairingVerdict === 'slot-taken') {
    return (
      <Hint tone="warning">
        The DevKit holds one pairing at a time and it belongs to another device. Only that device
        can give it up, using its own unpair action, and doing so erases the recordings on the
        card. If it is gone for good, the DevKit has to be reflashed with the unbond image.
      </Hint>
    );
  }

  return (
    <Hint tone="warning">
      Pairing did not complete. Tap Connect and accept the request on this iPhone.
      {pairing && pairing.lastSecurityError !== 0
        ? ` The device reported: ${describeSecurityError(pairing.lastSecurityError)}.`
        : ''}
    </Hint>
  );
}

function DeviceStatus({ connection }: { connection: ReturnType<typeof useOmiConnection> }) {
  const { status, device } = connection;

  if (status === 'connected') {
    const pairing = connection.pairing;
    return (
      <>
        <Row label="Status" value="Connected" />
        <Row label="Name" value={connection.client?.name ?? 'Omi DevKit'} />
        {pairing?.smpEnabled ? (
          <Row
            label="Pairing"
            value={
              pairing.linkEncrypted
                ? `Paired · ${pairing.bondCount} of ${pairing.maxBonds} slot${pairing.maxBonds === 1 ? '' : 's'}`
                : 'Not encrypted'
            }
            tone={pairing.linkEncrypted ? undefined : 'warning'}
          />
        ) : null}
        {connection.mtu ? <Row label="MTU" value={String(connection.mtu)} tone="muted" /> : null}
      </>
    );
  }

  if (device) {
    return (
      <>
        <View style={styles.availabilityRow}>
          <View style={styles.availability}>
            <StatusDot active />
            <Text style={styles.deviceName}>{device.name}</Text>
          </View>
          <Text style={styles.rssi}>{device.rssi !== null ? `${device.rssi} dBm` : ''}</Text>
        </View>
        <Text style={styles.availabilityHint}>Available to connect</Text>
      </>
    );
  }

  return (
    <View style={styles.availabilityRow}>
      <View style={styles.availability}>
        <StatusDot active={false} />
        <Text style={styles.deviceNameMuted}>{idleStatusLabel(status)}</Text>
      </View>
    </View>
  );
}

function RecordingRow({
  item,
  playerState,
  onToggle,
}: {
  item: RecordingItem;
  playerState: PlayerState | null;
  onToggle: () => void;
}) {
  const isActive = playerState?.chunkId === item.id;
  const isPlaying = isActive && playerState?.playing === true;
  const isLoading = isActive && playerState?.loading === true;

  const title = item.startEpoch
    ? formatClock(item.startEpoch)
    : `Segment ${item.seq} · part ${item.index + 1}`;

  const duration = isActive && playerState && playerState.durationSeconds > 0
    ? playerState.durationSeconds
    : item.approxSeconds;

  return (
    <Pressable
      onPress={onToggle}
      accessibilityRole="button"
      accessibilityLabel={`${isPlaying ? 'Pause' : 'Play'} ${title}`}
      style={({ pressed }) => [styles.recording, pressed && { opacity: 0.7 }]}
    >
      <View style={styles.playIcon}>
        <Text style={styles.playGlyph}>{isLoading ? '…' : isPlaying ? '❚❚' : '▶'}</Text>
      </View>

      <View style={styles.recordingBody}>
        <Text style={styles.recordingTitle}>{title}</Text>
        <Text style={styles.recordingMeta}>
          {formatDuration(duration)} · {formatBytes(item.byteLength)}
          {item.evicted ? ' · device copy gone' : item.segmentIncomplete ? ' · partial' : ''}
        </Text>
        {isActive && playerState?.error ? (
          <Text style={styles.recordingError}>{playerState.error}</Text>
        ) : null}
        {isActive && playerState && playerState.durationSeconds > 0 ? (
          <View style={styles.recordingProgress}>
            <ProgressBar value={playerState.positionSeconds / playerState.durationSeconds} />
          </View>
        ) : null}
      </View>

      {isActive && playerState && playerState.durationSeconds > 0 ? (
        <Text style={styles.recordingPosition}>{formatDuration(playerState.positionSeconds)}</Text>
      ) : null}
    </Pressable>
  );
}

function waitFor(predicate: () => boolean, timeoutMs: number): Promise<void> {
  return new Promise((resolve) => {
    const deadline = Date.now() + timeoutMs;
    const timer = setInterval(() => {
      if (predicate() || Date.now() > deadline) {
        clearInterval(timer);
        resolve();
      }
    }, 100);
  });
}

const styles = StyleSheet.create({
  screen: {
    flex: 1,
    backgroundColor: colors.background,
  },
  content: {
    paddingHorizontal: spacing.lg,
  },
  heading: {
    color: colors.text,
    fontSize: 30,
    fontWeight: '700',
    letterSpacing: -0.5,
  },
  subheading: {
    color: colors.textMuted,
    fontSize: 14,
    marginTop: spacing.xs,
    marginBottom: spacing.xl,
  },
  buttonRow: {
    flexDirection: 'row',
    gap: spacing.md,
    marginTop: spacing.lg,
  },
  grow: {
    flex: 1,
  },
  availabilityRow: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
  },
  availability: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: spacing.sm,
  },
  availabilityHint: {
    color: colors.textMuted,
    fontSize: 13,
    marginTop: spacing.xs,
  },
  deviceName: {
    color: colors.text,
    fontSize: 16,
    fontWeight: '600',
  },
  deviceNameMuted: {
    color: colors.textMuted,
    fontSize: 16,
  },
  rssi: {
    color: colors.textFaint,
    fontSize: 13,
    fontVariant: ['tabular-nums'],
  },
  progressBlock: {
    marginTop: spacing.lg,
    gap: spacing.sm,
  },
  progressMeta: {
    flexDirection: 'row',
    justifyContent: 'space-between',
  },
  progressText: {
    color: colors.textMuted,
    fontSize: 13,
  },
  progressSub: {
    color: colors.textFaint,
    fontSize: 12,
    fontVariant: ['tabular-nums'],
  },
  empty: {
    color: colors.textMuted,
    fontSize: 14,
    lineHeight: 20,
  },
  clearLabel: {
    color: colors.textMuted,
    fontSize: 13,
    fontWeight: '600',
  },
  releaseLabel: {
    color: colors.danger,
    fontSize: 13,
    fontWeight: '600',
    marginTop: spacing.sm,
  },
  list: {
    marginTop: spacing.md,
    gap: spacing.xs,
  },
  recording: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: spacing.md,
    paddingVertical: spacing.md,
    borderTopWidth: StyleSheet.hairlineWidth,
    borderTopColor: colors.cardBorder,
  },
  playIcon: {
    width: 38,
    height: 38,
    borderRadius: radius.sm,
    backgroundColor: colors.track,
    alignItems: 'center',
    justifyContent: 'center',
  },
  playGlyph: {
    color: colors.text,
    fontSize: 13,
  },
  recordingBody: {
    flex: 1,
    gap: 2,
  },
  recordingTitle: {
    color: colors.text,
    fontSize: 15,
    fontWeight: '500',
  },
  recordingMeta: {
    color: colors.textFaint,
    fontSize: 12,
    fontVariant: ['tabular-nums'],
  },
  recordingError: {
    color: colors.danger,
    fontSize: 12,
  },
  recordingProgress: {
    marginTop: spacing.xs,
  },
  recordingPosition: {
    color: colors.textMuted,
    fontSize: 12,
    fontVariant: ['tabular-nums'],
  },
});

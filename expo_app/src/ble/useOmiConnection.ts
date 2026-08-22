/**
 * The connection state machine. Scanning and connecting are deliberately
 * separate: the app scans continuously to report whether the device is in range,
 * but only ever connects when the user asks it to. There is no auto-connect and
 * no silent reconnect after a drop.
 */

import { useCallback, useEffect, useRef, useState } from 'react';
import { State as BluetoothState, type Subscription } from 'react-native-ble-plx';

import { bleManager, OmiClient, scanForDevices, type DiscoveredDevice } from './omiDevice';
import { diagnosePairing, type PairingStatus, type PairingVerdict } from './pairing';
import type { RingInfo } from './protocol';

/**
 * A scan callback only fires on an advertisement, never on absence, so "gone"
 * has to be inferred from silence. The DevKit advertises several times a second.
 */
const AVAILABILITY_TIMEOUT_MS = 10_000;

const FRESHNESS_TICK_MS = 1_000;

export type ConnectionStatus =
  | 'initialising'
  | 'bluetooth-off'
  | 'unauthorised'
  | 'unsupported'
  | 'scanning'
  | 'available'
  | 'connecting'
  /** Waiting for the iOS pairing prompt to be accepted. */
  | 'pairing'
  | 'connected'
  | 'disconnecting';

export interface AvailableDevice extends DiscoveredDevice {
  lastSeenAt: number;
}

/** Whether the link still has to be encrypted before anything on the device can be read. */
function needsEncrypting(verdict: PairingVerdict): boolean {
  return verdict !== 'ready' && verdict !== 'not-required';
}

/**
 * Thrown when the link could not be encrypted. Carries the device's own account of why, because
 * the remedies are opposites: a stale bond is cleared on the phone, a taken slot on the device.
 */
class PairingRequiredError extends Error {
  readonly verdict: PairingVerdict;

  readonly status: PairingStatus;

  constructor(verdict: PairingVerdict, status: PairingStatus) {
    super(`pairing required: ${verdict}`);
    this.name = 'PairingRequiredError';
    this.verdict = verdict;
    this.status = status;
  }
}

export interface ConnectionState {
  status: ConnectionStatus;
  device: AvailableDevice | null;
  client: OmiClient | null;
  info: RingInfo | null;
  mtu: number | null;
  error: string | null;
  clockSynced: boolean;
  /** Null on firmware with no pairing service, which encrypts nothing. */
  pairing: PairingStatus | null;
  pairingVerdict: PairingVerdict | null;
}

export interface OmiConnection extends ConnectionState {
  canConnect: boolean;
  canDisconnect: boolean;
  busy: boolean;
  connect: () => Promise<void>;
  disconnect: () => Promise<void>;
  refreshInfo: () => Promise<void>;
  /** Gives up the bond so another phone can pair. Erases the device's recordings. */
  releaseBond: () => Promise<void>;
  /** Registered by the screen so a disconnect can stop an in-flight sync first. */
  setBeforeDisconnect: (handler: (() => Promise<void>) | null) => void;
}

export function useOmiConnection(): OmiConnection {
  const [state, setState] = useState<ConnectionState>({
    status: 'initialising',
    device: null,
    client: null,
    info: null,
    mtu: null,
    error: null,
    clockSynced: false,
    pairing: null,
    pairingVerdict: null,
  });

  const clientRef = useRef<OmiClient | null>(null);
  const disconnectSubscription = useRef<Subscription | null>(null);
  const stopScanRef = useRef<(() => void) | null>(null);
  const beforeDisconnect = useRef<(() => Promise<void>) | null>(null);
  /** Set while a bond release is in flight, so the drop it causes is explained, not reported. */
  const releasing = useRef(false);
  const lastSeenRef = useRef<number>(0);
  const mounted = useRef(true);

  const patch = useCallback((next: Partial<ConnectionState>) => {
    if (mounted.current) {
      setState((current) => ({ ...current, ...next }));
    }
  }, []);

  const stopScan = useCallback(() => {
    stopScanRef.current?.();
    stopScanRef.current = null;
  }, []);

  const startScan = useCallback(() => {
    if (stopScanRef.current) {
      return;
    }
    stopScanRef.current = scanForDevices(
      (found) => {
        lastSeenRef.current = Date.now();
        setState((current) => {
          // A connected peripheral stops advertising, so ignore stragglers.
          if (
            current.status === 'connected' ||
            current.status === 'connecting' ||
            current.status === 'pairing'
          ) {
            return current;
          }
          // Advertisements arrive several times a second. Re-rendering on each
          // one would churn the whole screen, so only react to a change worth
          // showing: a different device, or a meaningful signal shift.
          const settled =
            current.status === 'available' &&
            current.device?.id === found.id &&
            Math.abs((current.device?.rssi ?? 0) - (found.rssi ?? 0)) < 5;
          if (settled) {
            return current;
          }
          return {
            ...current,
            status: 'available',
            error: null,
            device: { ...found, lastSeenAt: Date.now() },
          };
        });
      },
      (error) => {
        patch({ error: error.message });
      },
    );
  }, [patch]);

  // Bluetooth adapter lifecycle. Scanning is only legal while powered on.
  useEffect(() => {
    mounted.current = true;
    const manager = bleManager();

    const subscription = manager.onStateChange((bluetoothState) => {
      if (bluetoothState === BluetoothState.PoweredOn) {
        // Never resume scanning underneath a live connection.
        if (clientRef.current) {
          return;
        }
        setState((current) =>
          current.status === 'connected' ||
          current.status === 'connecting' ||
          current.status === 'pairing' ||
          current.status === 'disconnecting'
            ? current
            : { ...current, status: 'scanning', error: null },
        );
        startScan();
        return;
      }

      stopScan();
      if (bluetoothState === BluetoothState.Unauthorized) {
        patch({ status: 'unauthorised', device: null, error: null });
      } else if (bluetoothState === BluetoothState.PoweredOff) {
        patch({ status: 'bluetooth-off', device: null, error: null });
      } else if (bluetoothState === BluetoothState.Unsupported) {
        patch({ status: 'unsupported', device: null, error: null });
      }
    }, true);

    return () => {
      mounted.current = false;
      subscription.remove();
      stopScan();
      disconnectSubscription.current?.remove();
      clientRef.current?.disconnect().catch(() => {});
    };
  }, [patch, startScan, stopScan]);

  // Availability decay: nothing tells us the device left, so time it out.
  useEffect(() => {
    const timer = setInterval(() => {
      setState((current) => {
        if (current.status !== 'available') {
          return current;
        }
        if (Date.now() - lastSeenRef.current < AVAILABILITY_TIMEOUT_MS) {
          return current;
        }
        return { ...current, status: 'scanning', device: null };
      });
    }, FRESHNESS_TICK_MS);
    return () => clearInterval(timer);
  }, []);

  const handleUnexpectedDrop = useCallback(() => {
    disconnectSubscription.current?.remove();
    disconnectSubscription.current = null;
    clientRef.current = null;
    lastSeenRef.current = 0;
    patch({
      status: 'scanning',
      client: null,
      device: null,
      info: null,
      mtu: null,
      clockSynced: false,
      pairing: null,
      // A released device is unpaired from its side only; this phone still holds the key, so
      // the very next connect will fail until iOS is told to forget it. Say so now, while the
      // user still remembers asking for it.
      pairingVerdict: releasing.current ? 'stale-bond' : null,
      error: releasing.current ? null : 'The device disconnected',
    });
    releasing.current = false;
    startScan();
  }, [patch, startScan]);

  const connect = useCallback(async () => {
    const target = state.device;
    if (!target || clientRef.current) {
      return;
    }

    // Only the drop caused by the release it was set for may consume this; a release that never
    // dropped must not colour an unrelated disconnect much later.
    releasing.current = false;
    patch({ status: 'connecting', error: null });
    // iOS will not deliver connection callbacks reliably while a scan with
    // duplicates is running, and we do not need discovery once we have a target.
    stopScan();

    try {
      const client = await OmiClient.connect(target.id);
      clientRef.current = client;

      disconnectSubscription.current = client.onDisconnected(handleUnexpectedDrop);

      // Always safe to read, encrypted or not, and it is the only thing that can explain a
      // pairing failure -- so read it before anything that pairing could block.
      let pairing = await client.readPairingStatus();

      const initialVerdict = pairing ? diagnosePairing(pairing) : null;
      if (pairing && initialVerdict && needsEncrypting(initialVerdict)) {
        patch({ status: 'pairing', pairing, pairingVerdict: initialVerdict });
        await client.establishEncryption();
        // Re-read rather than assume: the reason code the device recorded is the only reliable
        // account of what just happened, and it is what tells a stale bond from a taken slot.
        pairing = (await client.readPairingStatus()) ?? pairing;
      }

      const verdict = pairing ? diagnosePairing(pairing) : null;
      if (pairing && verdict && needsEncrypting(verdict)) {
        throw new PairingRequiredError(verdict, pairing);
      }

      const clockSynced = await client.syncClock();
      const info = await client.readInfo();

      patch({
        status: 'connected',
        client,
        info,
        mtu: client.mtu,
        clockSynced,
        pairing,
        pairingVerdict: verdict,
        error: null,
      });
    } catch (error) {
      const failed = clientRef.current;
      clientRef.current = null;
      disconnectSubscription.current?.remove();
      disconnectSubscription.current = null;
      // The link is up whenever the failure came after connect(), and leaving it up would keep
      // the device out of its advertising state, so nothing could ever find it again.
      await failed?.disconnect().catch(() => {});

      const failure = error instanceof PairingRequiredError ? error : null;
      const verdict = failure?.verdict ?? null;
      patch({
        status: 'scanning',
        client: null,
        pairing: failure?.status ?? null,
        pairingVerdict: verdict,
        // A pairing verdict is explained in full by the UI; a bare "Insufficient Encryption"
        // on top of it would only be noise.
        error: verdict ? null : error instanceof Error ? error.message : String(error),
      });
      startScan();
    }
  }, [handleUnexpectedDrop, patch, startScan, state.device]);

  const disconnect = useCallback(async () => {
    const client = clientRef.current;
    if (!client) {
      return;
    }

    patch({ status: 'disconnecting' });

    // Stop an in-flight sync first so the device closes its read handle and
    // persists its offset rather than being cut off mid-transfer.
    try {
      await beforeDisconnect.current?.();
    } catch {
      // A failed stop must not block the disconnect.
    }

    disconnectSubscription.current?.remove();
    disconnectSubscription.current = null;
    clientRef.current = null;

    await client.disconnect();

    lastSeenRef.current = 0;
    patch({
      status: 'scanning',
      client: null,
      device: null,
      info: null,
      mtu: null,
      clockSynced: false,
      pairing: null,
      pairingVerdict: null,
      error: null,
    });
    startScan();
  }, [patch, startScan]);

  /**
   * Hands the device over. The firmware erases the card before releasing the bond and then drops
   * the link, so the disconnect handler does the cleanup and there is nothing to await.
   */
  const releaseBond = useCallback(async () => {
    const client = clientRef.current;
    if (!client) {
      return;
    }
    try {
      await beforeDisconnect.current?.();
    } catch {
      // A sync that will not stop must not block the release.
    }
    releasing.current = true;
    try {
      await client.releaseBond();
      // The write only asks. The device still has to erase the card before it unpairs and drops
      // the link, and that takes long enough that the screen must not keep saying "Connected".
      patch({ status: 'disconnecting', error: null });
    } catch (error) {
      releasing.current = false;
      patch({ error: error instanceof Error ? error.message : String(error) });
    }
  }, [patch]);

  const refreshInfo = useCallback(async () => {
    const client = clientRef.current;
    if (!client) {
      return;
    }
    try {
      patch({ info: await client.readInfo() });
    } catch (error) {
      patch({ error: error instanceof Error ? error.message : String(error) });
    }
  }, [patch]);

  const setBeforeDisconnect = useCallback((handler: (() => Promise<void>) | null) => {
    beforeDisconnect.current = handler;
  }, []);

  return {
    ...state,
    canConnect: state.status === 'available',
    canDisconnect: state.status === 'connected',
    busy:
      state.status === 'connecting' ||
      state.status === 'pairing' ||
      state.status === 'disconnecting',
    connect,
    disconnect,
    refreshInfo,
    releaseBond,
    setBeforeDisconnect,
  };
}

/**
 * The connection state machine. Scanning and connecting are deliberately
 * separate: the app scans continuously to report whether the device is in range,
 * but only ever connects when the user asks it to. There is no auto-connect and
 * no silent reconnect after a drop.
 */

import { useCallback, useEffect, useRef, useState } from 'react';
import { State as BluetoothState, type Subscription } from 'react-native-ble-plx';

import { bleManager, OmiClient, scanForDevices, type DiscoveredDevice } from './omiDevice';
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
  | 'connected'
  | 'disconnecting';

export interface AvailableDevice extends DiscoveredDevice {
  lastSeenAt: number;
}

export interface ConnectionState {
  status: ConnectionStatus;
  device: AvailableDevice | null;
  client: OmiClient | null;
  info: RingInfo | null;
  mtu: number | null;
  error: string | null;
  clockSynced: boolean;
}

export interface OmiConnection extends ConnectionState {
  canConnect: boolean;
  canDisconnect: boolean;
  busy: boolean;
  connect: () => Promise<void>;
  disconnect: () => Promise<void>;
  refreshInfo: () => Promise<void>;
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
  });

  const clientRef = useRef<OmiClient | null>(null);
  const disconnectSubscription = useRef<Subscription | null>(null);
  const stopScanRef = useRef<(() => void) | null>(null);
  const beforeDisconnect = useRef<(() => Promise<void>) | null>(null);
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
          if (current.status === 'connected' || current.status === 'connecting') {
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
      error: 'The device disconnected',
    });
    startScan();
  }, [patch, startScan]);

  const connect = useCallback(async () => {
    const target = state.device;
    if (!target || clientRef.current) {
      return;
    }

    patch({ status: 'connecting', error: null });
    // iOS will not deliver connection callbacks reliably while a scan with
    // duplicates is running, and we do not need discovery once we have a target.
    stopScan();

    try {
      const client = await OmiClient.connect(target.id);
      clientRef.current = client;

      disconnectSubscription.current = client.onDisconnected(handleUnexpectedDrop);

      const clockSynced = await client.syncClock();
      const info = await client.readInfo();

      patch({
        status: 'connected',
        client,
        info,
        mtu: client.mtu,
        clockSynced,
        error: null,
      });
    } catch (error) {
      clientRef.current = null;
      disconnectSubscription.current?.remove();
      disconnectSubscription.current = null;
      patch({
        status: 'scanning',
        client: null,
        error: error instanceof Error ? error.message : String(error),
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
      error: null,
    });
    startScan();
  }, [patch, startScan]);

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
    busy: state.status === 'connecting' || state.status === 'disconnecting',
    connect,
    disconnect,
    refreshInfo,
    setBeforeDisconnect,
  };
}
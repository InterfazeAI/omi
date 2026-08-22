/**
 * BLE client for the Omi DevKit v2 storage service.
 *
 * The device exposes one characteristic that is both the command channel and the
 * bulk data channel: you write 6-byte commands to 30295781 and every reply --
 * status bytes and 440-byte audio blocks alike -- comes back as a notification on
 * that same characteristic. Notifications are demultiplexed purely by length.
 */

import {
  BleATTErrorCode,
  BleError,
  BleManager,
  type Device,
  type Subscription,
} from 'react-native-ble-plx';

import { base64ToBytes, bytesToBase64 } from './bytes';
import {
  AUDIO_SERVICE_UUID,
  PAIRING_RELEASE_CHAR_UUID,
  PAIRING_SERVICE_UUID,
  PAIRING_STATUS_CHAR_UUID,
  SD_BLE_SIZE,
  STORAGE_CMD_CHAR_UUID,
  STORAGE_INFO_CHAR_UUID,
  STORAGE_SERVICE_UUID,
  StorageCommand,
  TIME_SYNC_SERVICE_UUID,
  TIME_SYNC_WRITE_CHAR_UUID,
  UNBOND_MAGIC,
} from './constants';
import { parsePairingStatus, type PairingStatus } from './pairing';
import {
  encodeCommand,
  encodeEpochSeconds,
  parseRingInfo,
  type RingInfo,
  type StorageNotification,
} from './protocol';

/** A 440-byte notification needs an ATT MTU of at least 443 (payload + ATT header). */
export const REQUIRED_MTU = SD_BLE_SIZE + 3;

const CONNECT_TIMEOUT_MS = 25_000;

/**
 * Service discovery, MTU and PHY negotiation need a moment to settle before the
 * first GATT operation. The reference client waits the same 2 s; skipping it
 * produces intermittent failures.
 */
const POST_CONNECT_SETTLE_MS = 2_000;

let manager: BleManager | null = null;

export function bleManager(): BleManager {
  if (!manager) {
    manager = new BleManager();
  }
  return manager;
}

export interface DiscoveredDevice {
  id: string;
  name: string;
  rssi: number | null;
}

/**
 * Scans for the DevKit. It advertises the audio service; the storage service is
 * not in the advertisement, so we filter on the audio UUID and discover storage
 * after connecting.
 *
 * `allowDuplicates` keeps advertisements flowing so the caller can tell "still in
 * range" from "seen once, then powered off".
 */
export function scanForDevices(
  onFound: (device: DiscoveredDevice) => void,
  onError: (error: Error) => void,
): () => void {
  const bm = bleManager();
  bm.startDeviceScan([AUDIO_SERVICE_UUID], { allowDuplicates: true }, (error, device) => {
    if (error) {
      onError(error);
      return;
    }
    if (device) {
      onFound({
        id: device.id,
        name: device.name ?? device.localName ?? 'Omi DevKit',
        rssi: device.rssi ?? null,
      });
    }
  });

  return () => {
    try {
      bm.stopDeviceScan();
    } catch {
      // The manager may already be torn down; nothing useful to do.
    }
  };
}

const delay = (ms: number) => new Promise<void>((resolve) => setTimeout(resolve, ms));

/**
 * How long to keep retrying an encrypted read while iOS shows its pairing prompt.
 *
 * The first read of an encrypted characteristic is what makes CoreBluetooth start pairing, and it
 * fails with `Insufficient Encryption` while the user is still deciding. The device's own SMP
 * timeout is 30 s, so there is no point waiting longer than that.
 */
const PAIRING_WAIT_MS = 25_000;

const PAIRING_RETRY_MS = 2_000;

/**
 * True when a GATT operation failed only because the link is not encrypted yet. On iOS this is
 * the signal that pairing is in progress rather than that anything is wrong.
 */
export function isEncryptionError(error: unknown): boolean {
  if (!(error instanceof BleError)) {
    return false;
  }
  return (
    error.attErrorCode === BleATTErrorCode.InsufficientEncryption ||
    error.attErrorCode === BleATTErrorCode.InsufficientAuthentication ||
    error.attErrorCode === BleATTErrorCode.InsufficientEncryptionKeySize
  );
}

export class OmiClient {
  private readonly device: Device;

  private monitor: Subscription | null = null;

  /** Negotiated ATT MTU. Below REQUIRED_MTU the device stalls silently on reads. */
  readonly mtu: number;

  private constructor(device: Device, mtu: number) {
    this.device = device;
    this.mtu = mtu;
  }

  get id(): string {
    return this.device.id;
  }

  get name(): string {
    return this.device.name ?? 'Omi DevKit';
  }

  static async connect(deviceId: string): Promise<OmiClient> {
    const bm = bleManager();

    // requestMTU is honoured on Android and ignored on iOS, where CoreBluetooth
    // negotiates automatically (typically 527, comfortably above what we need).
    const device = await bm.connectToDevice(deviceId, {
      timeout: CONNECT_TIMEOUT_MS,
      requestMTU: 512,
      autoConnect: false,
    });

    await device.discoverAllServicesAndCharacteristics();
    await delay(POST_CONNECT_SETTLE_MS);

    return new OmiClient(device, device.mtu ?? 0);
  }

  onDisconnected(handler: () => void): Subscription {
    return this.device.onDisconnected(() => handler());
  }

  async isConnected(): Promise<boolean> {
    try {
      return await this.device.isConnected();
    } catch {
      return false;
    }
  }

  /** Reads and decodes the 21-byte storage info characteristic. */
  async readInfo(): Promise<RingInfo> {
    const characteristic = await this.device.readCharacteristicForService(
      STORAGE_SERVICE_UUID,
      STORAGE_INFO_CHAR_UUID,
    );
    if (!characteristic.value) {
      throw new Error('storage info characteristic returned no value');
    }
    return parseRingInfo(base64ToBytes(characteristic.value));
  }

  /**
   * Reads the pairing status. Never needs an encrypted link, by design.
   *
   * Returns null on firmware that predates the pairing service, which has nothing to encrypt and
   * so needs no pairing at all.
   */
  async readPairingStatus(): Promise<PairingStatus | null> {
    try {
      const characteristic = await this.device.readCharacteristicForService(
        PAIRING_SERVICE_UUID,
        PAIRING_STATUS_CHAR_UUID,
      );
      return characteristic.value ? parsePairingStatus(base64ToBytes(characteristic.value)) : null;
    } catch {
      return null;
    }
  }

  /**
   * Gets the link encrypted, pairing if that is what it takes.
   *
   * There is no API to ask CoreBluetooth to pair: touching an encrypted characteristic is the
   * trigger, and iOS raises its prompt in response to the device's refusal. So the way to pair is
   * to keep attempting the read that pairing would make succeed, until it does or the device's
   * 30 s SMP window closes.
   */
  async establishEncryption(): Promise<boolean> {
    const deadline = Date.now() + PAIRING_WAIT_MS;

    for (;;) {
      try {
        await this.readInfo();
        return true;
      } catch (error) {
        if (!isEncryptionError(error) || Date.now() >= deadline) {
          return false;
        }
        await delay(PAIRING_RETRY_MS);
      }
    }
  }

  /**
   * Hands the device to a new owner, which **erases every recording on the card** first so the
   * next owner cannot read them. Needs the encrypted link: only the current owner may do this.
   *
   * The device wipes, then unpairs, then drops the connection, so expect a disconnect rather than
   * a tidy return. iOS keeps its half of the bond and must be told to forget the device.
   */
  async releaseBond(): Promise<void> {
    await this.device.writeCharacteristicWithResponseForService(
      PAIRING_SERVICE_UUID,
      PAIRING_RELEASE_CHAR_UUID,
      bytesToBase64(new Uint8Array([...UNBOND_MAGIC].map((c) => c.charCodeAt(0)))),
    );
  }

  /**
   * Sets the device clock so future index records carry wall-clock time.
   * Best effort: the characteristic only exists on firmware that supports it,
   * and a missing clock only costs us nice timestamps.
   */
  async syncClock(now = new Date()): Promise<boolean> {
    try {
      await this.device.writeCharacteristicWithResponseForService(
        TIME_SYNC_SERVICE_UUID,
        TIME_SYNC_WRITE_CHAR_UUID,
        bytesToBase64(encodeEpochSeconds(now.getTime() / 1000)),
      );
      return true;
    } catch {
      return false;
    }
  }

  /**
   * Subscribes to the storage characteristic. Must be called before issuing a
   * READ, otherwise the first blocks arrive with nobody listening.
   */
  subscribe(
    onNotification: (notification: StorageNotification) => void,
    onError: (error: Error) => void,
  ): void {
    this.unsubscribe();
    this.monitor = this.device.monitorCharacteristicForService(
      STORAGE_SERVICE_UUID,
      STORAGE_CMD_CHAR_UUID,
      (error, characteristic) => {
        if (error) {
          // A cancelled monitor during teardown is expected, not a failure.
          if (!/cancelled|destroyed|disconnected/i.test(error.message ?? '')) {
            onError(error);
          }
          return;
        }
        if (!characteristic?.value) {
          return;
        }
        const bytes = base64ToBytes(characteristic.value);
        if (bytes.length === 1) {
          onNotification({ kind: 'status', status: bytes[0] });
        } else if (bytes.length > 1) {
          onNotification({ kind: 'data', bytes });
        }
      },
    );
  }

  unsubscribe(): void {
    this.monitor?.remove();
    this.monitor = null;
  }

  /** Writes a 6-byte command frame. The device notifies its result separately. */
  async sendCommand(command: number, segment: number, offset = 0): Promise<void> {
    await this.device.writeCharacteristicWithResponseForService(
      STORAGE_SERVICE_UUID,
      STORAGE_CMD_CHAR_UUID,
      bytesToBase64(encodeCommand(command, segment, offset)),
    );
  }

  /**
   * Best-effort STOP. The firmware persists its offset and closes the read
   * handle here, so it is worth attempting even on the success path -- but the
   * write often fails while notifications still saturate the link, and that is
   * not an error worth surfacing.
   *
   * `segment` must be a segment that exists: the firmware validates the segment
   * number for every opcode, so STOP against segment 0 is rejected outright.
   */
  async stopTransfer(segment: number): Promise<void> {
    try {
      await this.sendCommand(StorageCommand.Stop, Math.max(1, segment));
    } catch {
      // Ignored deliberately; see above.
    }
  }

  async disconnect(): Promise<void> {
    this.unsubscribe();
    try {
      await bleManager().cancelDeviceConnection(this.device.id);
    } catch {
      // Already gone.
    }
  }
}

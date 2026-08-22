/**
 * The device's pairing-status characteristic, and what to do about what it says.
 *
 * Source of truth: `pairing_status_read_handler()` in omi/firmware/devkit/src/transport.c.
 *
 * Link security is opt-in in the firmware (`secure-pairing.conf`). In a build with it on,
 * everything that carries content -- audio, recordings, the clock, DFU -- needs a paired,
 * encrypted link, and this characteristic is the one thing that does not. That is deliberate: it
 * exists to explain why pairing failed, and a diagnostic you can only read once pairing works
 * would be useless. So the app can always read it, on any build, paired or not, and decide what
 * to tell the user.
 *
 * No native imports here, so this is directly unit-testable.
 */

import { PAIRING_STATUS_BYTES } from './constants';

export interface PairingStatus {
  /** Layout version. Refuse anything newer rather than misreading it. */
  version: number;
  /** The firmware was built with the Security Manager, so content is encrypted. */
  smpEnabled: boolean;
  bondable: boolean;
  /** Bonds survive a reboot. Without this the device forgets you every power cycle. */
  bondsPersist: boolean;
  /** This connection is encrypted, i.e. we are the bonded device. */
  linkEncrypted: boolean;
  bondCount: number;
  /** `CONFIG_BT_MAX_PAIRED`, which is 1 on this board: one owner at a time. */
  maxBonds: number;
  lastPairingError: number;
  lastSecurityError: number;
  lastSecurityLevel: number;
  securityLevel: number;
  connections: number;
  pairingSuccesses: number;
  pairingFailures: number;
  unbondRequests: number;
  lastUnbondResult: number;
}

/** `bt_security_err`, from zephyr/include/zephyr/bluetooth/conn.h. */
export const SecurityError = {
  Success: 0,
  AuthenticationFail: 1,
  /** The peer offered a key the device no longer has: the classic stale bond. */
  PinOrKeyMissing: 2,
  OobNotAvailable: 3,
  AuthRequirements: 4,
  PairNotSupported: 5,
  /** Usually means there is no free bond slot. */
  PairNotAllowed: 6,
  InvalidParam: 7,
  KeyRejected: 8,
  Unspecified: 9,
} as const;

export function describeSecurityError(code: number): string {
  switch (code) {
    case SecurityError.Success:
      return 'no error';
    case SecurityError.AuthenticationFail:
      return 'authentication failed';
    case SecurityError.PinOrKeyMissing:
      return 'the phone offered a pairing key the device no longer has';
    case SecurityError.OobNotAvailable:
      return 'out-of-band data not available';
    case SecurityError.AuthRequirements:
      return 'authentication requirements not met';
    case SecurityError.PairNotSupported:
      return 'pairing not supported';
    case SecurityError.PairNotAllowed:
      return 'pairing not allowed, usually because no bond slot is free';
    case SecurityError.InvalidParam:
      return 'invalid pairing parameters';
    case SecurityError.KeyRejected:
      return 'pairing key rejected';
    case SecurityError.Unspecified:
      return 'unspecified, which on this board means pairing timed out rather than being refused';
    default:
      return `unknown security error ${code}`;
  }
}

function readU32LE(bytes: Uint8Array, at: number): number {
  return (
    (bytes[at] | (bytes[at + 1] << 8) | (bytes[at + 2] << 16) | (bytes[at + 3] << 24)) >>> 0
  );
}

export function parsePairingStatus(bytes: Uint8Array): PairingStatus {
  if (bytes.length < PAIRING_STATUS_BYTES) {
    throw new Error(`pairing status too short: ${bytes.length} bytes`);
  }

  const flags = bytes[1];
  return {
    version: bytes[0],
    smpEnabled: (flags & 0x01) !== 0,
    bondable: (flags & 0x02) !== 0,
    bondsPersist: (flags & 0x04) !== 0,
    linkEncrypted: (flags & 0x08) !== 0,
    bondCount: bytes[2],
    maxBonds: bytes[3],
    lastPairingError: bytes[4],
    lastSecurityError: bytes[5],
    lastSecurityLevel: bytes[6],
    securityLevel: bytes[7],
    connections: readU32LE(bytes, 8),
    pairingSuccesses: readU32LE(bytes, 12),
    pairingFailures: readU32LE(bytes, 16),
    unbondRequests: readU32LE(bytes, 20),
    // Signed: it is an errno from bt_unpair().
    lastUnbondResult: (bytes[24] << 24) >> 24,
  };
}

export type PairingVerdict =
  /** This build encrypts nothing, so there is nothing to pair. */
  | 'not-required'
  /** Encrypted link established: the device is ours. */
  | 'ready'
  /** A free slot is waiting; accepting the iOS prompt is all it takes. */
  | 'needs-pairing'
  /** iOS holds a key the device threw away. Only forgetting it on the phone clears this. */
  | 'stale-bond'
  /** Another device owns the one bond slot. */
  | 'slot-taken';

/**
 * What the status means for the user, in the order that matters.
 *
 * The board allows exactly one bond, so the two ways to be locked out look almost identical from
 * the phone and have opposite remedies: a stale bond is fixed on the phone, a taken slot only on
 * the device. Guessing wrong sends the user to the wrong place, so the reason code decides.
 */
export function diagnosePairing(status: PairingStatus): PairingVerdict {
  if (!status.smpEnabled) {
    return 'not-required';
  }
  if (status.linkEncrypted) {
    return 'ready';
  }
  // The device kept no key for us, yet the phone thinks it has one. Releasing a bond the device
  // does not have cannot help; the phone has to drop its half.
  if (status.bondCount === 0 && status.lastSecurityError === SecurityError.PinOrKeyMissing) {
    return 'stale-bond';
  }
  if (status.bondCount >= status.maxBonds) {
    return 'slot-taken';
  }
  return 'needs-pairing';
}

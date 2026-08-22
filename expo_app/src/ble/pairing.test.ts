import assert from 'node:assert/strict';
import test from 'node:test';

import { diagnosePairing, parsePairingStatus, SecurityError, type PairingStatus } from './pairing';

/** Builds the 25 bytes the device would return, so the parser is tested against the wire layout. */
function raw(fields: {
  flags?: number;
  bondCount?: number;
  maxBonds?: number;
  lastPairingError?: number;
  lastSecurityError?: number;
  lastSecurityLevel?: number;
  securityLevel?: number;
  connections?: number;
  successes?: number;
  failures?: number;
  unbondRequests?: number;
  lastUnbondResult?: number;
} = {}): Uint8Array {
  const bytes = new Uint8Array(25);
  const view = new DataView(bytes.buffer);
  bytes[0] = 1;
  bytes[1] = fields.flags ?? 0;
  bytes[2] = fields.bondCount ?? 0;
  bytes[3] = fields.maxBonds ?? 1;
  bytes[4] = fields.lastPairingError ?? 0;
  bytes[5] = fields.lastSecurityError ?? 0;
  bytes[6] = fields.lastSecurityLevel ?? 0;
  bytes[7] = fields.securityLevel ?? 1;
  view.setUint32(8, fields.connections ?? 0, true);
  view.setUint32(12, fields.successes ?? 0, true);
  view.setUint32(16, fields.failures ?? 0, true);
  view.setUint32(20, fields.unbondRequests ?? 0, true);
  view.setInt8(24, fields.lastUnbondResult ?? 0);
  return bytes;
}

function status(overrides: Partial<PairingStatus> = {}): PairingStatus {
  return {
    ...parsePairingStatus(raw({ flags: 0x07 })),
    ...overrides,
  };
}

test('parses the flag bits and counters the firmware packs into the status', () => {
  const parsed = parsePairingStatus(
    raw({
      flags: 0x0f,
      bondCount: 1,
      maxBonds: 1,
      lastSecurityError: SecurityError.PinOrKeyMissing,
      lastSecurityLevel: 1,
      securityLevel: 2,
      connections: 300_000,
      successes: 2,
      failures: 5,
      unbondRequests: 1,
      lastUnbondResult: -22,
    }),
  );

  assert.equal(parsed.version, 1);
  assert.equal(parsed.smpEnabled, true);
  assert.equal(parsed.bondable, true);
  assert.equal(parsed.bondsPersist, true);
  assert.equal(parsed.linkEncrypted, true);
  assert.equal(parsed.bondCount, 1);
  assert.equal(parsed.securityLevel, 2);
  assert.equal(parsed.connections, 300_000);
  assert.equal(parsed.pairingFailures, 5);
  // errno comes back signed; reading it unsigned would report 234 and mean nothing.
  assert.equal(parsed.lastUnbondResult, -22);
});

test('rejects a truncated status instead of reading past the end', () => {
  assert.throws(() => parsePairingStatus(new Uint8Array(24)), /too short/);
});

test('tolerates a longer status, as the characteristic is expected to grow', () => {
  const grown = new Uint8Array(40);
  grown.set(raw({ flags: 0x01, bondCount: 1 }));
  assert.equal(parsePairingStatus(grown).smpEnabled, true);
});

test('a build without the Security Manager needs no pairing at all', () => {
  assert.equal(diagnosePairing(status({ smpEnabled: false, linkEncrypted: false })), 'not-required');
});

test('an encrypted link is ready even with the slot count full, because the slot is ours', () => {
  assert.equal(
    diagnosePairing(status({ linkEncrypted: true, bondCount: 1, maxBonds: 1 })),
    'ready',
  );
});

test('a free slot just needs the user to accept the prompt', () => {
  assert.equal(diagnosePairing(status({ bondCount: 0, maxBonds: 1 })), 'needs-pairing');
});

test('key-missing with no bond on the device is the phone holding a stale bond', () => {
  // The remedy is on the phone, so this must not be reported as a slot problem: the device has
  // no bond to release and telling the user to release one leaves them stuck.
  assert.equal(
    diagnosePairing(
      status({ bondCount: 0, lastSecurityError: SecurityError.PinOrKeyMissing }),
    ),
    'stale-bond',
  );
});

test('a full slot on an unencrypted link means another device owns the device', () => {
  assert.equal(
    diagnosePairing(
      status({
        bondCount: 1,
        maxBonds: 1,
        linkEncrypted: false,
        lastSecurityError: SecurityError.PairNotAllowed,
      }),
    ),
    'slot-taken',
  );
});

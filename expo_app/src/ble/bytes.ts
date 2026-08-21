/** react-native-ble-plx moves characteristic values as base64 strings. */

import { fromByteArray, toByteArray } from 'base64-js';

export function base64ToBytes(value: string): Uint8Array {
  return toByteArray(value);
}

export function bytesToBase64(bytes: Uint8Array): string {
  return fromByteArray(bytes);
}

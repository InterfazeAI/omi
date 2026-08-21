#ifndef STORAGE_SERVICE_H
#define STORAGE_SERVICE_H

#include <Arduino.h>
#include <BLEServer.h>

/// Ring-buffer offline storage protocol on service 30295780, as spoken by the
/// Flutter app from firmware 3.0.20 onwards (app/lib/services/devices/
/// ring_protocol.dart, pinned in app/test/unit/ring_protocol_test.dart).
///
///   30295781  Write + Notify  commands in, protocol notifications out
///   30295782  Read            16-byte status snapshot, four uint32 LE
///
/// Commands (big-endian, unlike the status read):
///   0x03 stop sync            1 byte
///   0x10 info                 1 byte
///   0x11 read                 [0x11][start_seq u64] or [...][count u32]
///   0x12 advance              [0x12][new_read_seq u64]
///   0x13 clear                1 byte
///
/// Notifications:
///   0x01 ACK        [0x01][status]
///   0x02 INFO       [0x02][read u64][write u64][cap u32][dropped u64][pkt u16]
///   0x03 DATA       [0x03][raw bytes] — deliberately NOT record-aligned
///   0x04 DONE       [0x04][status][next_seq u64]
///   0x05 READ_BEGIN [0x05][start_seq u64][packet_count u32]

void storage_service_init(BLEServer *server);

/// Advance the protocol by one step: answer a pending command, or push one batch
/// of a running transfer. Returns quickly so the caller can interleave draining
/// the record write queue; call it in a loop from the storage task.
void storage_service_process();

/// True from CMD_RING_READ until DONE/STOP.
bool storage_service_transfer_active();

/// True while the app is mid-conversation about a sync — from the CMD_RING_INFO
/// that snapshots the cursors, through the CMD_RING_READ that quotes that
/// snapshot back, until the transfer ends.
///
/// The live drain must stand down for all of it, not merely while a transfer
/// runs. The app reads `read_seq` with INFO and then sends READ starting from
/// that value; a cursor that moves in between makes the start_seq stale, and
/// start_pending_read rejects it as SEQ_OUT_OF_RANGE, so the app sees no DATA and
/// gives up on its timeout. The latch expires on its own so a peer that asks for
/// INFO and then goes quiet cannot switch live audio off indefinitely.
bool storage_service_sync_busy();

void storage_service_on_connect(uint16_t conn_id);
void storage_service_on_disconnect();

/// Recompute the cached 16-byte status served by a read of 30295782. A GATT read
/// must never block on SD I/O, so the characteristic value is refreshed here on
/// a timer instead.
void storage_service_refresh_status(bool force);

#endif // STORAGE_SERVICE_H

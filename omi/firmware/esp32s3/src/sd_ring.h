#ifndef SD_RING_H
#define SD_RING_H

#include <Arduino.h>
#include <stdint.h>

/// Fixed-capacity ring of 444-byte audio records on a FAT-formatted SD card.
///
/// Records are addressed by a monotonic 64-bit sequence number; the byte offset
/// of a record is `(seq % capacity_packets) * RING_RECORD_BYTES` inside one
/// fixed-size file, so the FAT allocation chain stops changing after the first
/// lap and the card sees a steady rewrite pattern.
///
/// `read_seq` is the single delivery cursor: a record below it has been handed
/// to the phone (over live audio or a ring sync), a record at or above it has
/// not. Advancing it marks data consumed, it does not erase anything — the bytes
/// stay readable until the write pointer laps them.
///
/// Every entry point is serialised on an internal mutex, but the SD and FAT
/// layers are slow enough that callers should still keep to the storage task.

#define SD_RING_OK 0
#define SD_RING_ERR_RANGE (-1)     ///< seq outside [read_seq, write_seq]
#define SD_RING_ERR_IO (-2)        ///< card read/write failed
#define SD_RING_ERR_NOT_READY (-3) ///< no card mounted
#define SD_RING_ERR_INVAL (-4)     ///< bad argument

#define SD_RING_SEQ_INVALID UINT64_MAX

typedef struct {
    uint64_t read_seq;
    uint64_t write_seq;
    uint64_t dropped_packets;
    uint32_t capacity_packets;
} sd_ring_info_t;

/// Mount the card and open (creating if needed) the ring and its metadata.
/// Formats the card when it carries no usable filesystem, so a corrupted card
/// self-heals into a working state instead of bricking offline recording.
bool sd_ring_init();

bool sd_ring_is_ready();

void sd_ring_get_info(sd_ring_info_t *out);

/// UTC epoch persisted alongside the ring metadata at last power-down, or 0.
uint32_t sd_ring_restored_epoch();

/// Mark every record written from here on as carrying an authoritative
/// timestamp. Call once the app has synced the clock.
///
/// Not persisted, and deliberately so: after a power cycle the restored epoch is
/// stale again by however long the switch was off, so nothing written before the
/// next sync can be trusted either.
void sd_ring_mark_timestamps_authoritative();

/// True only when every record still unread carries an authoritative timestamp.
///
/// This is the question the app's `rtc_valid` status bit is really asking. The
/// live clock being synced is not sufficient: a backlog recorded before the sync
/// is stamped from the restored estimate, and reporting those as trustworthy
/// files offline audio at a wall-clock time in the past.
bool sd_ring_timestamps_authoritative();

/// Append one RING_RECORD_BYTES record. Returns the sequence number it landed
/// at, or SD_RING_SEQ_INVALID on failure. On overrun the read cursor is
/// force-advanced past the overwritten records and `dropped_packets` grows by
/// the same amount — recording never stalls waiting for the phone.
uint64_t sd_ring_write_record(const uint8_t *record);

/// Read consecutive records starting at `start_seq`. Stops at `write_seq` and at
/// the ring's wrap boundary, so a short read is normal and not an error.
int sd_ring_read(uint64_t start_seq, uint8_t *buf, uint32_t max_bytes, uint32_t *bytes_read, uint32_t *packets_read);

/// Move the delivery cursor forward and publish it immediately. `new_read_seq`
/// must lie in [read_seq, write_seq]; anything else is SD_RING_ERR_RANGE.
int sd_ring_advance(uint64_t new_read_seq);

/// Same, but leave publishing to the next sd_ring_tick. Used by the live drain,
/// which advances once per record: forcing a flush ten times a second would cost
/// far more card traffic than the worst case it protects against, which is
/// re-delivering under a second of audio after an abrupt power cut.
int sd_ring_advance_lazy(uint64_t new_read_seq);

/// Drop everything: read/write/dropped all return to 0. The data file is kept so
/// its allocation is reused.
int sd_ring_clear();

/// Flush pending data and republish metadata when the cadence is due (or when
/// `force`). Pass the current UTC epoch, or 0 when the clock has no basis, so
/// the next boot can restore an estimate.
void sd_ring_tick(uint32_t epoch, bool force);

#endif // SD_RING_H

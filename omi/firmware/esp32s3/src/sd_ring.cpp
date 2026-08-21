#include "sd_ring.h"

#include <FS.h>
#include <SD.h>
#include <SPI.h>

#include "config.h"

#define RING_META_MAGIC 0x4F4D4952U // 'OMIR'
#define RING_META_VERSION 1U

struct __attribute__((packed)) ring_meta {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint64_t generation;
    uint64_t read_seq;
    uint64_t write_seq;
    uint64_t dropped_packets;
    uint32_t capacity_packets;
    uint32_t last_epoch;
    uint32_t crc32;
};

static SPIClass sd_spi(FSPI);
static File data_file;
static File meta_file;
static SemaphoreHandle_t ring_mutex = nullptr;

static bool ring_ready = false;
static uint64_t ring_read_seq = 0;
static uint64_t ring_write_seq = 0;
static uint64_t ring_dropped = 0;
static uint32_t ring_capacity = 0;
static uint32_t ring_restored_epoch_value = 0;
static uint32_t ring_last_epoch = 0;

/// First sequence number written after the app synced the clock, or
/// SD_RING_SEQ_INVALID while nothing this boot has a trustworthy timestamp.
static uint64_t authoritative_from = SD_RING_SEQ_INVALID;

static uint64_t meta_generation = 0;
static uint8_t meta_next_slot = 0;

/// Highest byte offset the data file has been written up to, tracked in RAM.
///
/// File::size() cannot answer this. Writes go through buffered stdio, and the
/// size it reports comes from stat(), which on FATFS reads the directory entry —
/// and FatFs only updates that on a sync. Every record after the first in a
/// flush window would therefore look like it was leaving a gap, and the gap-fill
/// in sd_ring_write_record would seek backwards and zero the records already
/// written. Silent, and it would take out most of the first lap.
static uint32_t data_extent = 0;

static uint32_t records_since_sync = 0;
static uint32_t last_sync_ms = 0;
static bool data_dirty = false; ///< records written but not yet flushed to card
static bool meta_dirty = false; ///< cursors moved but not yet published

static uint8_t meta_slot_buf[RING_META_SLOT_BYTES];

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static uint32_t crc32_of(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320U & (uint32_t) (-(int32_t) (crc & 1U)));
        }
    }
    return ~crc;
}

static bool ring_lock()
{
    return ring_mutex != nullptr && xSemaphoreTakeRecursive(ring_mutex, portMAX_DELAY) == pdTRUE;
}

static void ring_unlock()
{
    if (ring_mutex != nullptr) {
        xSemaphoreGiveRecursive(ring_mutex);
    }
}

static bool meta_valid(const struct ring_meta *m)
{
    if (m->magic != RING_META_MAGIC || m->version != RING_META_VERSION) {
        return false;
    }
    if (m->capacity_packets < RING_MIN_PACKETS) {
        return false;
    }
    if (m->read_seq > m->write_seq) {
        return false;
    }
    const uint32_t expect = crc32_of((const uint8_t *) m, sizeof(*m) - sizeof(uint32_t));
    return expect == m->crc32;
}

static bool meta_write_slot(uint8_t slot, uint32_t epoch)
{
    struct ring_meta record = {};
    record.magic = RING_META_MAGIC;
    record.version = RING_META_VERSION;
    record.generation = ++meta_generation;
    record.read_seq = ring_read_seq;
    record.write_seq = ring_write_seq;
    record.dropped_packets = ring_dropped;
    record.capacity_packets = ring_capacity;
    record.last_epoch = epoch;
    record.crc32 = crc32_of((const uint8_t *) &record, sizeof(record) - sizeof(uint32_t));

    memset(meta_slot_buf, 0, sizeof(meta_slot_buf));
    memcpy(meta_slot_buf, &record, sizeof(record));

    if (!meta_file.seek((uint32_t) slot * RING_META_SLOT_BYTES)) {
        meta_generation--;
        return false;
    }
    if (meta_file.write(meta_slot_buf, sizeof(meta_slot_buf)) != sizeof(meta_slot_buf)) {
        meta_generation--;
        return false;
    }
    meta_file.flush();
    ring_last_epoch = epoch;
    return true;
}

// Publish the in-RAM cursors. The data file is flushed first so that metadata
// never claims a record the card has not actually taken: after a power cut the
// ring is short by at most the records written since the last publish, never
// corrupt.
static bool meta_publish(uint32_t epoch)
{
    if (!ring_ready) {
        return false;
    }
    if (data_dirty) {
        data_file.flush();
        data_dirty = false;
    }
    const uint8_t slot = meta_next_slot;
    if (!meta_write_slot(slot, epoch)) {
        Serial.println("SD ring: metadata write failed");
        return false;
    }
    meta_next_slot = (uint8_t) ((slot + 1U) % RING_META_SLOTS);
    records_since_sync = 0;
    meta_dirty = false;
    last_sync_ms = millis();
    return true;
}

static bool meta_load()
{
    struct ring_meta best = {};
    bool found = false;
    uint8_t best_slot = 0;

    for (uint8_t slot = 0; slot < RING_META_SLOTS; slot++) {
        if (!meta_file.seek((uint32_t) slot * RING_META_SLOT_BYTES)) {
            continue;
        }
        if (meta_file.read(meta_slot_buf, sizeof(meta_slot_buf)) != (int) sizeof(meta_slot_buf)) {
            continue;
        }
        struct ring_meta candidate;
        memcpy(&candidate, meta_slot_buf, sizeof(candidate));
        if (!meta_valid(&candidate)) {
            continue;
        }
        if (!found || candidate.generation > best.generation) {
            best = candidate;
            best_slot = slot;
            found = true;
        }
    }

    if (!found) {
        return false;
    }

    ring_read_seq = best.read_seq;
    ring_write_seq = best.write_seq;
    ring_dropped = best.dropped_packets;
    ring_capacity = best.capacity_packets;
    ring_restored_epoch_value = best.last_epoch;
    ring_last_epoch = best.last_epoch;
    meta_generation = best.generation;
    meta_next_slot = (uint8_t) ((best_slot + 1U) % RING_META_SLOTS);
    return true;
}

static uint32_t compute_capacity()
{
    uint64_t total = SD.totalBytes();
    if (total == 0) {
        return 0;
    }
    uint64_t budget = (total / 100ULL) * (uint64_t) RING_CARD_FRACTION_PCT;
    if (budget > RING_MAX_BYTES) {
        budget = RING_MAX_BYTES;
    }
    const uint64_t packets = budget / RING_RECORD_BYTES;
    if (packets < RING_MIN_PACKETS) {
        return 0;
    }
    return (uint32_t) packets;
}

static bool open_rw(const char *path, File &out)
{
    if (!SD.exists(path)) {
        File created = SD.open(path, FILE_WRITE);
        if (!created) {
            return false;
        }
        created.close();
    }
    out = SD.open(path, "r+");
    return (bool) out;
}

static bool mount_card()
{
    sd_spi.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);

    // format_if_empty = true: a card whose filesystem is unreadable gets rebuilt
    // rather than taking offline recording down with it.
    if (SD.begin(SD_CS_PIN, sd_spi, SD_SPI_FREQ_HZ, "/sd", 8, true)) {
        return true;
    }

    Serial.println("SD ring: mount at full speed failed, retrying at 4MHz");
    SD.end();
    if (SD.begin(SD_CS_PIN, sd_spi, 4000000, "/sd", 8, true)) {
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool sd_ring_init()
{
    if (ring_mutex == nullptr) {
        ring_mutex = xSemaphoreCreateRecursiveMutex();
        if (ring_mutex == nullptr) {
            return false;
        }
    }
    if (ring_ready) {
        return true;
    }

    if (!mount_card()) {
        Serial.println("SD ring: no card (offline recording disabled)");
        return false;
    }

    if (!open_rw(RING_META_PATH, meta_file)) {
        Serial.println("SD ring: cannot open metadata file");
        SD.end();
        return false;
    }
    if (!open_rw(RING_DATA_PATH, data_file)) {
        Serial.println("SD ring: cannot open data file");
        meta_file.close();
        SD.end();
        return false;
    }

    ring_ready = true;
    // Nothing is buffered yet, so this is the one moment the filesystem's own
    // size is trustworthy.
    data_extent = (uint32_t) data_file.size();

    if (!meta_load()) {
        ring_read_seq = 0;
        ring_write_seq = 0;
        ring_dropped = 0;
        ring_restored_epoch_value = 0;
        meta_generation = 0;
        meta_next_slot = 0;
        ring_capacity = compute_capacity();
        if (ring_capacity == 0) {
            Serial.println("SD ring: card too small for a usable ring");
            data_file.close();
            meta_file.close();
            SD.end();
            ring_ready = false;
            return false;
        }
        if (!meta_publish(0)) {
            Serial.println("SD ring: could not initialise metadata");
            data_file.close();
            meta_file.close();
            SD.end();
            ring_ready = false;
            return false;
        }
        Serial.printf("SD ring: initialised, capacity %u packets\n", (unsigned) ring_capacity);
    } else {
        Serial.printf("SD ring: resumed read=%llu write=%llu dropped=%llu capacity=%u\n",
                      (unsigned long long) ring_read_seq,
                      (unsigned long long) ring_write_seq,
                      (unsigned long long) ring_dropped,
                      (unsigned) ring_capacity);
    }

    last_sync_ms = millis();
    Serial.printf("SD ring: card %llu MB, ring %llu MB\n",
                  (unsigned long long) (SD.totalBytes() / (1024ULL * 1024ULL)),
                  (unsigned long long) ((uint64_t) ring_capacity * RING_RECORD_BYTES / (1024ULL * 1024ULL)));
    return true;
}

bool sd_ring_is_ready()
{
    return ring_ready;
}

void sd_ring_get_info(sd_ring_info_t *out)
{
    if (out == nullptr) {
        return;
    }
    if (!ring_lock()) {
        memset(out, 0, sizeof(*out));
        return;
    }
    out->read_seq = ring_read_seq;
    out->write_seq = ring_write_seq;
    out->dropped_packets = ring_dropped;
    out->capacity_packets = ring_capacity;
    ring_unlock();
}

uint32_t sd_ring_restored_epoch()
{
    return ring_restored_epoch_value;
}

void sd_ring_mark_timestamps_authoritative()
{
    if (!ring_lock()) {
        return;
    }
    // Only the first sync matters. Later re-syncs just correct drift, and moving
    // the watermark forward would wrongly demote records already stamped from a
    // clock that was already good.
    if (authoritative_from == SD_RING_SEQ_INVALID) {
        authoritative_from = ring_write_seq;
    }
    ring_unlock();
}

bool sd_ring_timestamps_authoritative()
{
    if (!ring_lock()) {
        return false;
    }
    const bool ok = authoritative_from != SD_RING_SEQ_INVALID && ring_read_seq >= authoritative_from;
    ring_unlock();
    return ok;
}

uint64_t sd_ring_write_record(const uint8_t *record)
{
    if (record == nullptr) {
        return SD_RING_SEQ_INVALID;
    }
    if (!ring_ready || !ring_lock()) {
        return SD_RING_SEQ_INVALID;
    }

    const uint64_t seq = ring_write_seq;
    const uint32_t offset = (uint32_t) (seq % ring_capacity) * RING_RECORD_BYTES;

    // Writes march forward from offset 0 and only ever revisit offsets after a
    // full lap, so the file is extended by append and never sparsely seeked. The
    // gap-fill is a safety net for a file left short by a power cut.
    const uint32_t size = data_extent;
    if (offset > size) {
        Serial.printf(
            "SD ring: unexpected gap (offset %u > size %u), realigning\n", (unsigned) offset, (unsigned) size);
        if (!data_file.seek(size)) {
            ring_unlock();
            return SD_RING_SEQ_INVALID;
        }
        static uint8_t zeros[RING_RECORD_BYTES];
        memset(zeros, 0, sizeof(zeros));
        uint32_t remaining = offset - size;
        while (remaining > 0) {
            const uint32_t step = remaining > RING_RECORD_BYTES ? RING_RECORD_BYTES : remaining;
            if (data_file.write(zeros, step) != step) {
                ring_unlock();
                return SD_RING_SEQ_INVALID;
            }
            remaining -= step;
        }
    } else if (!data_file.seek(offset)) {
        ring_unlock();
        return SD_RING_SEQ_INVALID;
    }

    if (data_file.write(record, RING_RECORD_BYTES) != RING_RECORD_BYTES) {
        Serial.println("SD ring: record write failed");
        ring_unlock();
        return SD_RING_SEQ_INVALID;
    }
    if (offset + RING_RECORD_BYTES > data_extent) {
        data_extent = offset + RING_RECORD_BYTES;
    }
    data_dirty = true;

    const uint64_t new_write_seq = seq + 1U;
    if (new_write_seq - ring_read_seq > (uint64_t) ring_capacity) {
        const uint64_t overflow = (new_write_seq - ring_read_seq) - (uint64_t) ring_capacity;
        ring_read_seq += overflow;
        ring_dropped += overflow;
    }
    ring_write_seq = new_write_seq;
    records_since_sync++;
    meta_dirty = true;

    ring_unlock();
    return seq;
}

int sd_ring_read(uint64_t start_seq, uint8_t *buf, uint32_t max_bytes, uint32_t *bytes_read, uint32_t *packets_read)
{
    if (buf == nullptr || bytes_read == nullptr || packets_read == nullptr) {
        return SD_RING_ERR_INVAL;
    }
    *bytes_read = 0;
    *packets_read = 0;

    if (!ring_ready || !ring_lock()) {
        return SD_RING_ERR_NOT_READY;
    }

    if (start_seq < ring_read_seq || start_seq > ring_write_seq) {
        ring_unlock();
        return SD_RING_ERR_RANGE;
    }

    uint64_t want = max_bytes / RING_RECORD_BYTES;
    const uint64_t available = ring_write_seq - start_seq;
    if (want > available) {
        want = available;
    }

    // One seek cannot span the modulo wrap; a short read here is normal and the
    // caller simply comes back for the rest.
    const uint32_t index = (uint32_t) (start_seq % ring_capacity);
    const uint32_t to_end = ring_capacity - index;
    if (want > to_end) {
        want = to_end;
    }

    if (want == 0) {
        ring_unlock();
        return SD_RING_OK;
    }

    // Records only become readable once metadata has published them, and the
    // data file is flushed before that happens, so nothing served here can be a
    // torn write from the last power cut.
    if (data_dirty) {
        data_file.flush();
        data_dirty = false;
    }

    const uint32_t offset = index * RING_RECORD_BYTES;
    const uint32_t length = (uint32_t) want * RING_RECORD_BYTES;
    if (!data_file.seek(offset)) {
        ring_unlock();
        return SD_RING_ERR_IO;
    }
    const int got = data_file.read(buf, length);
    if (got <= 0) {
        ring_unlock();
        return SD_RING_ERR_IO;
    }

    *packets_read = (uint32_t) got / RING_RECORD_BYTES;
    *bytes_read = *packets_read * RING_RECORD_BYTES;
    ring_unlock();
    return SD_RING_OK;
}

static int ring_advance_locked(uint64_t new_read_seq, bool publish)
{
    if (!ring_ready || !ring_lock()) {
        return SD_RING_ERR_NOT_READY;
    }
    if (new_read_seq < ring_read_seq || new_read_seq > ring_write_seq) {
        ring_unlock();
        return SD_RING_ERR_RANGE;
    }
    ring_read_seq = new_read_seq;
    meta_dirty = true;
    bool ok = true;
    if (publish) {
        ok = meta_publish(ring_last_epoch);
    }
    ring_unlock();
    return ok ? SD_RING_OK : SD_RING_ERR_IO;
}

int sd_ring_advance(uint64_t new_read_seq)
{
    return ring_advance_locked(new_read_seq, true);
}

int sd_ring_advance_lazy(uint64_t new_read_seq)
{
    return ring_advance_locked(new_read_seq, false);
}

int sd_ring_clear()
{
    if (!ring_ready || !ring_lock()) {
        return SD_RING_ERR_NOT_READY;
    }
    ring_read_seq = 0;
    ring_write_seq = 0;
    ring_dropped = 0;
    // Sequence numbers restart, so the watermark has to follow them down or every
    // record written after this clear would read as pre-sync.
    if (authoritative_from != SD_RING_SEQ_INVALID) {
        authoritative_from = 0;
    }
    const bool ok = meta_publish(ring_last_epoch);
    ring_unlock();
    return ok ? SD_RING_OK : SD_RING_ERR_IO;
}

void sd_ring_tick(uint32_t epoch, bool force)
{
    if (!ring_ready || !ring_lock()) {
        return;
    }

    const uint32_t now = millis();
    const bool elapsed = (now - last_sync_ms) >= RING_SYNC_INTERVAL_MS;

    // The persisted epoch only has to be good enough to seed a boot-time
    // estimate, so it gets a far slower cadence than the cursors. Treating a
    // moved epoch as urgent would republish metadata every RING_SYNC_INTERVAL_MS
    // forever, since wall time advances every second whether or not the device
    // is recording — a permanent 1 Hz write to the same two sectors.
    const bool epoch_moved = epoch != 0 && epoch != ring_last_epoch;
    const bool epoch_due = epoch_moved && (now - last_sync_ms) >= RING_EPOCH_PERSIST_INTERVAL_MS;
    const bool due = force || records_since_sync >= RING_SYNC_RECORDS || (meta_dirty && elapsed) || epoch_due;

    if (due) {
        meta_publish(epoch != 0 ? epoch : ring_last_epoch);
    }
    ring_unlock();
}

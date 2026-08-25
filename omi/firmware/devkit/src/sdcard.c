#include "sdcard.h"

#include <errno.h>
#include <ff.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/fs_sys.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/sys/check.h>

#include "rtc.h"

LOG_MODULE_REGISTER(sdcard, CONFIG_LOG_DEFAULT_LEVEL);

static FATFS fat_fs;

static struct fs_mount_t mount_point = {
    .type = FS_FATFS,
    .fs_data = &fat_fs,
};

// dt_flags is 16 bits and carries only devicetree-level flags (active level, drive strength).
// GPIO_INT_DISABLE lives above bit 16, so it truncated to 0 and the compiler warned about it on
// every build. Active high is what gpio_pin_set_dt() below assumes, and 0 is active high, so the
// behaviour was right by accident. The real configuration is passed to gpio_pin_configure_dt().
// Same fix as button.c; see DEBUGGING.md on the dt_flags trap.
struct gpio_dt_spec sd_en_gpio_pin = {.port = DEVICE_DT_GET(DT_NODELABEL(gpio0)), .pin = 19, .dt_flags = 0};

#define MAX_PATH_LENGTH 32
static char current_full_path[MAX_PATH_LENGTH];
static char read_buffer[MAX_PATH_LENGTH];
static char write_buffer[MAX_PATH_LENGTH];

uint32_t file_num_array[2];

static const char *disk_mount_pt = "/SD:/";

bool sd_enabled = false;

// Serializes every filesystem operation. FatFs is built here without FF_FS_LOCK, so the
// recording thread and the BLE sync thread must not be inside the FS at the same time.
// Zephyr mutexes are recursive for the owning thread, so nested public calls are safe.
static K_MUTEX_DEFINE(sd_mutex);

// The audio file stays open for the lifetime of the mount. Re-opening per 440-byte block
// rewrote the FAT directory entry ~12 times a second, which is both slow and a wide window
// for an unclean reset to lose the file length.
static struct fs_file_t audio_file;
static bool audio_file_open = false;
static uint32_t audio_file_size = 0;

// Audio arrives 440 bytes at a time, which is the worst possible shape for an SD card: each
// one lands inside a single 512-byte sector, so the card erases and reprograms a whole
// internal page per write. Measured at ~220ms each on this card, which is within the 250ms
// the SD spec allows for a single block. Batching into a large sector-aligned run turns
// those into one sequential multi-block write. The buffer is the amount of audio a power
// cut can cost (~4.6s at the current bitrate); the periodic sync bounds it no further.
#define WRITE_BATCH_SIZE 8192
static uint8_t write_batch[WRITE_BATCH_SIZE];
static uint32_t write_batch_len = 0;

// A sync serves 440 bytes per BLE notification. Reading that little straight from the card
// meant an open/seek/read/close per packet, so the transfer spent most of its time in FatFs
// rather than on the air. Pull a large run into RAM instead and serve packets from it.
// An exact multiple of the 440-byte BLE packet, so refills never split a packet in two.
#define READ_CACHE_SIZE (440 * 18)
static uint8_t read_cache[READ_CACHE_SIZE];
static uint32_t read_cache_start = 0;
static uint32_t read_cache_len = 0;
static struct fs_file_t read_file;
static bool read_file_open = false;
// Whether read_buffer points at the segment still being appended to; only then can a read
// need audio that is still sitting in the write batch.
static bool read_target_is_newest = true;

// Recording is a ring of fixed-size segment files rather than one growing file, so it never
// has to stop: when the budget is reached the oldest segment is unlinked. Segments are used
// instead of wrapping inside a single file because every write then stays a sequential append,
// which is what the batching below is tuned for, and because recovering the write position
// after a reset is just "the highest-numbered file" rather than persisted state.
//
// lcm(WRITE_BATCH_SIZE, 440). A segment boundary that is a multiple of both means a batch
// flush never straddles two segments and the 440-byte block grid never shifts within one.
#define SEG_ALIGN 450560u
#define SEG_MIN_BYTES (8u * SEG_ALIGN)   // ~3.4 MB, ~25 min of audio
#define SEG_MAX_BYTES (512u * SEG_ALIGN) // ~220 MB, ~25 h of audio
// Eviction drops a whole segment, so this is really "how coarse is the loss": at 128 segments
// you lose <1% of retained audio at a time. file_num on the wire is 8-bit, hence the ceiling.
#ifndef CONFIG_OMI_RING_SEGMENT_COUNT
#define CONFIG_OMI_RING_SEGMENT_COUNT 128
#endif
#ifndef CONFIG_OMI_RING_SEGMENT_BYTES
#define CONFIG_OMI_RING_SEGMENT_BYTES 0
#endif
#define SEG_TARGET_COUNT ((uint32_t) CONFIG_OMI_RING_SEGMENT_COUNT)
#define SEG_MAX_COUNT 200u
// Leave the card room for FAT metadata and the sidecars rather than filling it exactly.
#define SEG_BUDGET_NUMERATOR 9u
#define SEG_BUDGET_DENOMINATOR 10u
// How many over-budget segments mount may reclaim before handing the rest to rotation. Mount
// runs with nothing feeding the watchdog, so this bounds the worst case there.
#define SEG_BOOT_TRIM_MAX 4u

#define AUDIO_DIR "/SD:/audio"
#define SEG_NAME_DIGITS 5
#define SEG_SEQ_MAX 99999u

static uint32_t seg_bytes = SEG_MIN_BYTES; // target size for a newly opened segment
static uint32_t seg_max_count = 1;         // how many segments we keep before evicting

// Sequence numbers of the segments on the card, ascending. Held explicitly rather than as a
// first/last pair because the host may delete any segment after syncing it, and inferring
// position from "oldest + n" silently addresses the wrong file once there is a hole.
static uint32_t seg_seq[SEG_MAX_COUNT];
static uint8_t seg_count = 0;
static uint32_t seg_oldest = 1; // mirrors seg_seq[0]
static uint32_t seg_newest = 1; // mirrors seg_seq[seg_count - 1]; the segment being appended to

// Eviction runs deep inside the write path, where the only report channel is a log the low
// priority log thread may never get scheduled to emit. Counting it and publishing it on the
// info characteristic makes a ring that has stopped evicting visible from the host.
static uint32_t ring_evictions = 0;
static int32_t ring_last_evict_err = 0;

// Same reasoning for the two operations that carry the recording itself. When a card stops
// accepting the append, the only symptom a host can see is a byte count that never moves --
// indistinguishable from a dead microphone without the errno. Read without the mutex on
// purpose: these matter most when an operation is stuck holding it.
static uint32_t io_open_failures = 0;
static uint32_t io_write_failures = 0;
static int32_t io_last_open_err = 0;
static int32_t io_last_write_err = 0;

static void seg_refresh_bounds(void)
{
    if (seg_count == 0) {
        seg_oldest = seg_newest;
        return;
    }
    seg_oldest = seg_seq[0];
    seg_newest = seg_seq[seg_count - 1];
}

// Removes entry `idx` from the ordered list. Caller must hold sd_mutex.
static void seg_forget_at(uint8_t idx)
{
    if (idx >= seg_count) {
        return;
    }
    for (uint8_t i = idx; i + 1 < seg_count; i++) {
        seg_seq[i] = seg_seq[i + 1];
    }
    seg_count--;
    seg_refresh_bounds();
}

#define BOOT_ID_PATH "/SD:/boot.bin"

#define INDEX_RECORD_SIZE 16
#define INDEX_INTERVAL_S 30

static uint32_t index_next_uptime_s = 0;

static void storage_close_locked(void);

// Segment paths are built per call rather than cached: two threads read them and a stale one
// silently addresses the wrong segment after a rotation.
static void seg_audio_path(char *dst, size_t n, uint32_t seq)
{
    snprintf(dst, n, AUDIO_DIR "/a%05u.txt", seq);
}

static void seg_index_path(char *dst, size_t n, uint32_t seq)
{
    snprintf(dst, n, AUDIO_DIR "/a%05u.idx", seq);
}

static int storage_open_locked(void)
{
    if (audio_file_open) {
        return 0;
    }

    char path[MAX_PATH_LENGTH];
    seg_audio_path(path, sizeof(path), seg_newest);

    fs_file_t_init(&audio_file);
    int rc = fs_open(&audio_file, path, FS_O_WRITE | FS_O_APPEND | FS_O_CREATE);
    if (rc) {
        io_open_failures++;
        io_last_open_err = rc;
        LOG_ERR("Failed to open %s for append: %d", path, rc);
        return rc;
    }

    off_t end = fs_seek(&audio_file, 0, FS_SEEK_END);
    if (end < 0) {
        LOG_ERR("Failed to seek to end of audio file: %d", (int) end);
        fs_close(&audio_file);
        return (int) end;
    }

    // The size the rest of the firmware sees counts audio already captured, including what is
    // still sitting in the batch, so index offsets stay continuous across a reopen.
    audio_file_size = (uint32_t) fs_tell(&audio_file) + write_batch_len;
    audio_file_open = true;
    LOG_INF("Audio file open, resuming at %u bytes", audio_file_size);
    return 0;
}

// fs_sync() cannot report success on an SD card in this SDK: card_ioctl()
// (zephyr/subsys/sd/sd_ops.c) has no break after its DISK_IOCTL_CTRL_SYNC case, so it falls
// into default: and overwrites the result with -ENOTSUP, which FatFs reports as -EIO. The
// flush itself has already happened by then -- f_sync writes the window, FAT and directory
// entry before that final ioctl, and sdmmc_wait_ready() does run. So the code carries no
// information and must not be acted on; treating it as fatal closed the file under every
// sync and stopped recording entirely. Genuine write failures still surface via fs_write.
static uint32_t sync_error_count = 0;

static void storage_sync_locked(void)
{
    if (!audio_file_open) {
        return;
    }
    if (fs_sync(&audio_file)) {
        sync_error_count++;
    }
}

// The same bug reaches any operation that ends in a flush: f_unlink and f_rename both finish
// with a CTRL_SYNC ioctl, so they report -EIO after having done the work. Believing that code
// cost a full debugging session -- eviction "failed" on every rotation while the file was in
// fact being deleted, so the ring grew without bound. Ask the filesystem what happened rather
// than trusting the return value.
static bool path_absent_locked(const char *path)
{
    struct fs_dirent probe;
    return fs_stat(path, &probe) == -ENOENT;
}

static void read_cache_invalidate_locked(void);
static int index_append_locked(void);

// Unlinks the segment at list position `idx` and forgets it. Caller must hold sd_mutex.
static int segment_drop_at_locked(uint8_t idx)
{
    if (idx >= seg_count) {
        return -ENOENT;
    }
    uint32_t seq = seg_seq[idx];

    char audio[MAX_PATH_LENGTH];
    char index[MAX_PATH_LENGTH];
    seg_audio_path(audio, sizeof(audio), seq);
    seg_index_path(index, sizeof(index), seq);

    // The append handle must not be pointing at a file about to be unlinked, or recording
    // silently stops until reboot: the handle survives but writes to a dead inode.
    if (seq == seg_newest) {
        write_batch_len = 0;
        storage_close_locked();
    }
    // Same for the sync thread's read handle. An in-flight download of this segment ends
    // early, which is correct: the bytes it was still asking for no longer exist.
    if (read_file_open && strcmp(read_buffer, audio) == 0) {
        LOG_WRN("dropping segment %u while it was being read; transfer ends early", seq);
        read_cache_invalidate_locked();
    }

    int rc = fs_unlink(audio);
    if (rc && !path_absent_locked(audio)) {
        // Genuinely still there. Keep it in the list: forgetting a file that is still present
        // would leak it forever and slowly fill the card with segments nothing will evict.
        ring_last_evict_err = rc;
        LOG_ERR("failed to unlink segment %u (%s): %d", seq, audio, rc);
        return rc;
    }
    fs_unlink(index); // may legitimately not exist

    ring_evictions++;
    seg_forget_at(idx);
    LOG_INF("dropped segment %u, %u remain", seq, seg_count);
    return 0;
}

static void segment_evict_oldest_locked(void)
{
    segment_drop_at_locked(0);
}

// Closes the current segment and starts the next one, evicting as needed to stay in budget.
// Caller must hold sd_mutex.
static int segment_rotate_locked(void)
{
    storage_close_locked();

    if (seg_newest >= SEG_SEQ_MAX) {
        // 99999 segments is decades of recording; wrapping the name would reorder the ring.
        LOG_ERR("segment sequence exhausted, recording stops");
        return -ENOSPC;
    }

    // Evict before appending so the list never has to hold more than SEG_MAX_COUNT entries.
    while (seg_count > 0 && (uint32_t) seg_count >= seg_max_count) {
        uint8_t before = seg_count;
        segment_evict_oldest_locked();
        if (seg_count == before) {
            break; // eviction failed; do not spin
        }
    }
    if (seg_count >= SEG_MAX_COUNT) {
        LOG_ERR("segment list full, cannot rotate");
        return -ENOSPC;
    }

    seg_seq[seg_count++] = seg_newest + 1u;
    seg_refresh_bounds();
    // Whatever a reader is pointed at, it is now a sealed segment: the new one did not exist
    // when the target was chosen.
    read_target_is_newest = false;

    // Deliberately not zeroed here: storage_open_locked() recomputes it as the new file's
    // length plus the still-pending batch, which is the invariant the rest of the code needs.
    int rc = storage_open_locked();
    if (rc) {
        LOG_ERR("failed to open new segment %u: %d", seg_newest, rc);
        return rc;
    }

    // Anchor the new segment in time immediately; without this its first index record would
    // not appear for up to INDEX_INTERVAL_S and its early audio could not be placed.
    index_next_uptime_s = 0;
    index_append_locked();

    LOG_INF("rotated to segment %u (%u of max %u segments, %u byte target)",
            seg_newest,
            seg_count,
            seg_max_count,
            seg_bytes);
    return 0;
}

// Commits the batch to the card. Everything that reads the file, or that needs the on-card
// length to be current, must call this first: until it runs the newest audio exists only in
// RAM. Caller must hold sd_mutex.
static int write_batch_flush_locked(void)
{
    if (write_batch_len == 0) {
        return 0;
    }

    int rc = storage_open_locked();
    if (rc) {
        return rc;
    }

    // audio_file_size already counts the pending batch, so it is what the file will measure
    // once this flush lands. Rotate before writing rather than splitting the batch, so a
    // 440-byte block never spans two segments; the batch then belongs to the new segment,
    // which storage_open_locked() accounts for when it recomputes the size. Segments can
    // therefore finish up to one batch short of the target, which is fine because every
    // consumer takes a segment's length from the filesystem.
    if (audio_file_size > seg_bytes) {
        rc = segment_rotate_locked();
        if (rc) {
            return rc;
        }
    }

    ssize_t written = fs_write(&audio_file, write_batch, write_batch_len);
    if (written < 0) {
        io_write_failures++;
        io_last_write_err = (int32_t) written;
        LOG_ERR("audio write failed: %d", (int) written);
        // The buffered audio is unrecoverable, but keeping it would wedge every later flush
        // behind the same failure. Drop the handle so the next write retries from a clean open.
        audio_file_size -= write_batch_len;
        write_batch_len = 0;
        storage_close_locked();
        return (int) written;
    }
    if ((uint32_t) written != write_batch_len) {
        LOG_ERR("short audio write: %d of %u bytes (card full?)", (int) written, write_batch_len);
        audio_file_size -= (write_batch_len - (uint32_t) written);
    }
    write_batch_len = 0;

    storage_sync_locked();
    return 0;
}

static void storage_close_locked(void)
{
    if (!audio_file_open) {
        return;
    }
    storage_sync_locked();
    fs_close(&audio_file);
    audio_file_open = false;
}

int storage_flush(void)
{
    k_mutex_lock(&sd_mutex, K_FOREVER);
    int rc = write_batch_flush_locked();
    k_mutex_unlock(&sd_mutex);
    return rc;
}

uint32_t storage_get_size(void)
{
    k_mutex_lock(&sd_mutex, K_FOREVER);
    uint32_t size = audio_file_size;
    k_mutex_unlock(&sd_mutex);
    return size;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

int storage_load_boot_id(uint32_t *out)
{
    if (out == NULL) {
        return -EINVAL;
    }

    k_mutex_lock(&sd_mutex, K_FOREVER);
    struct fs_file_t f;
    fs_file_t_init(&f);
    int rc = fs_open(&f, BOOT_ID_PATH, FS_O_READ);
    if (rc) {
        k_mutex_unlock(&sd_mutex);
        return rc;
    }
    uint8_t buf[4] = {0};
    rc = fs_read(&f, buf, sizeof(buf));
    fs_close(&f);
    k_mutex_unlock(&sd_mutex);

    if (rc < (int) sizeof(buf)) {
        return -EIO;
    }
    *out = get_le32(buf);
    return 0;
}

int storage_save_boot_id(uint32_t id)
{
    uint8_t buf[4];
    put_le32(buf, id);

    k_mutex_lock(&sd_mutex, K_FOREVER);
    struct fs_file_t f;
    fs_file_t_init(&f);
    int rc = fs_open(&f, BOOT_ID_PATH, FS_O_WRITE | FS_O_CREATE);
    if (rc) {
        LOG_ERR("boot id open failed: %d", rc);
        k_mutex_unlock(&sd_mutex);
        return rc;
    }
    rc = fs_seek(&f, 0, FS_SEEK_SET);
    if (rc == 0) {
        rc = fs_write(&f, buf, sizeof(buf));
    }
    fs_sync(&f);
    fs_close(&f);
    k_mutex_unlock(&sd_mutex);

    return rc < 0 ? rc : 0;
}

// Appends one index record. Caller must hold sd_mutex.
static int index_append_locked(void)
{
    uint8_t rec[INDEX_RECORD_SIZE];
    put_le32(rec + 0, audio_file_size);
    put_le32(rec + 4, rtc_get_epoch()); // 0 while the clock is unsynchronized
    put_le32(rec + 8, rtc_get_uptime_s());
    put_le32(rec + 12, rtc_get_boot_id());

    char path[MAX_PATH_LENGTH];
    seg_index_path(path, sizeof(path), seg_newest);

    struct fs_file_t f;
    fs_file_t_init(&f);
    int rc = fs_open(&f, path, FS_O_WRITE | FS_O_APPEND | FS_O_CREATE);
    if (rc) {
        LOG_ERR("index open failed: %d", rc);
        return rc;
    }
    if (fs_seek(&f, 0, FS_SEEK_END) >= 0) {
        rc = fs_write(&f, rec, sizeof(rec));
        if (rc < 0) {
            LOG_ERR("index write failed: %d", rc);
        }
    }
    fs_sync(&f);
    fs_close(&f);

    index_next_uptime_s = rtc_get_uptime_s() + INDEX_INTERVAL_S;
    return rc < 0 ? rc : 0;
}

#define INDEX_MARK_PENDING BIT(0)
#define INDEX_MARK_FORCE BIT(1)
static atomic_t index_mark_request;

void storage_index_mark(bool force)
{
    // Latch only. The clock is set from a GATT write handler, which runs on the BT RX thread
    // with a 1 KB stack -- writing a record from there is a FatFs call that does not fit and
    // faults the device (DEBUGGING.md trap 7). Deferring here rather than at the call site
    // keeps every future caller safe too, whatever thread it runs on.
    atomic_or(&index_mark_request, force ? (INDEX_MARK_PENDING | INDEX_MARK_FORCE) : INDEX_MARK_PENDING);
}

void storage_index_service(void)
{
    atomic_val_t request = atomic_clear(&index_mark_request);
    if (!(request & INDEX_MARK_PENDING)) {
        return;
    }

    k_mutex_lock(&sd_mutex, K_FOREVER);
    if ((request & INDEX_MARK_FORCE) || rtc_get_uptime_s() >= index_next_uptime_s) {
        index_append_locked();
    }
    k_mutex_unlock(&sd_mutex);
}

// Parses "aNNNNN.txt" into its sequence number. FatFs is built without long filenames, so
// readdir hands back 8.3 names in upper case; match either case.
static bool seg_parse_name(const char *name, uint32_t *seq)
{
    if ((name[0] != 'a' && name[0] != 'A')) {
        return false;
    }
    uint32_t value = 0;
    for (int i = 1; i <= SEG_NAME_DIGITS; i++) {
        if (name[i] < '0' || name[i] > '9') {
            return false;
        }
        value = value * 10u + (uint32_t) (name[i] - '0');
    }
    const char *ext = name + 1 + SEG_NAME_DIGITS;
    if (ext[0] != '.') {
        return false;
    }
    if (!((ext[1] == 't' || ext[1] == 'T') && (ext[2] == 'x' || ext[2] == 'X') && (ext[3] == 't' || ext[3] == 'T') &&
          ext[4] == '\0')) {
        return false;
    }
    *seq = value;
    return true;
}

// Sizes the ring to the card actually fitted, so the same firmware behaves sensibly on a 4 GB
// card and a 64 GB one. Segment size is derived rather than stored: existing segments keep
// whatever length they were written with, and only new ones follow the current target.
static void segment_size_budget_locked(uint64_t existing_bytes)
{
    if (CONFIG_OMI_RING_SEGMENT_BYTES > 0) {
        uint32_t forced = (uint32_t) CONFIG_OMI_RING_SEGMENT_BYTES;
        forced -= forced % SEG_ALIGN;
        seg_bytes = forced < SEG_ALIGN ? SEG_ALIGN : forced;
        seg_max_count = SEG_TARGET_COUNT;
        LOG_INF("ring: %u segments of %u bytes (forced by Kconfig)", seg_max_count, seg_bytes);
        return;
    }

    struct fs_statvfs st;
    int rc = fs_statvfs("/SD:", &st);
    if (rc) {
        LOG_ERR("statvfs failed: %d, falling back to minimum segment size", rc);
        seg_bytes = SEG_MIN_BYTES;
        seg_max_count = SEG_TARGET_COUNT;
        return;
    }

    uint64_t free_bytes = (uint64_t) st.f_bfree * (uint64_t) st.f_frsize;
    uint64_t budget = ((free_bytes + existing_bytes) * SEG_BUDGET_NUMERATOR) / SEG_BUDGET_DENOMINATOR;

    uint64_t target = budget / SEG_TARGET_COUNT;
    target -= target % SEG_ALIGN;
    if (target < SEG_MIN_BYTES) {
        target = SEG_MIN_BYTES;
    } else if (target > SEG_MAX_BYTES) {
        target = SEG_MAX_BYTES;
    }
    seg_bytes = (uint32_t) target;

    uint64_t count = budget / seg_bytes;
    if (count < 2) {
        count = 2; // a ring of one cannot evict without discarding what is being written
    } else if (count > SEG_MAX_COUNT) {
        count = SEG_MAX_COUNT;
    }
    seg_max_count = (uint32_t) count;

    LOG_INF("ring: %u segments of %u bytes (%llu MB free)", seg_max_count, seg_bytes, free_bytes >> 20);
}

// Rebuilds ring state from what is on the card. Caller must hold sd_mutex.
static int segment_scan_locked(void)
{
    // A card written by the pre-ring firmware has a single a01.txt. Rename rather than ignore
    // it, so an upgrade keeps the recording instead of silently orphaning it.
    struct fs_dirent legacy;
    if (fs_stat(AUDIO_DIR "/a01.txt", &legacy) == 0) {
        char to[MAX_PATH_LENGTH];
        seg_audio_path(to, sizeof(to), 1);
        // Judge by whether the source is gone, not by the return code: see path_absent_locked.
        fs_rename(AUDIO_DIR "/a01.txt", to);
        if (path_absent_locked(AUDIO_DIR "/a01.txt")) {
            LOG_INF("migrated legacy a01.txt to %s", to);
            seg_index_path(to, sizeof(to), 1);
            fs_rename(AUDIO_DIR "/a01.idx", to);
        } else {
            LOG_ERR("could not migrate legacy a01.txt");
        }
    }

    struct fs_dir_t dir;
    fs_dir_t_init(&dir);
    int rc = fs_opendir(&dir, AUDIO_DIR);
    if (rc) {
        LOG_ERR("cannot open %s: %d", AUDIO_DIR, rc);
        return rc;
    }

    seg_count = 0;
    uint64_t total = 0;
    uint32_t skipped = 0;
    while (1) {
        struct fs_dirent entry;
        if (fs_readdir(&dir, &entry) != 0 || entry.name[0] == '\0') {
            break;
        }
        uint32_t seq;
        if (entry.type != FS_DIR_ENTRY_FILE || !seg_parse_name(entry.name, &seq)) {
            continue;
        }
        if (seg_count >= SEG_MAX_COUNT) {
            skipped++;
            continue;
        }
        total += entry.size;
        // Insertion sort: readdir order is not defined, and everything downstream indexes this
        // list oldest-first.
        uint8_t at = seg_count;
        while (at > 0 && seg_seq[at - 1] > seq) {
            seg_seq[at] = seg_seq[at - 1];
            at--;
        }
        seg_seq[at] = seq;
        seg_count++;
    }
    fs_closedir(&dir);

    if (skipped) {
        LOG_ERR("%u segments beyond the %u the ring can track were ignored", skipped, SEG_MAX_COUNT);
    }

    segment_size_budget_locked(total);

    if (seg_count == 0) {
        seg_seq[0] = 1;
        seg_count = 1;
        seg_refresh_bounds();
        char path[MAX_PATH_LENGTH];
        seg_audio_path(path, sizeof(path), 1);
        LOG_INF("no segments yet, creating %s", path);
        struct fs_file_t f;
        fs_file_t_init(&f);
        rc = fs_open(&f, path, FS_O_WRITE | FS_O_CREATE);
        if (rc) {
            LOG_ERR("failed to create first segment: %d", rc);
            return rc;
        }
        fs_close(&f);
        return 0;
    }

    seg_refresh_bounds();
    LOG_INF("found %u segments, %u..%u, %llu MB total", seg_count, seg_oldest, seg_newest, total >> 20);

    // A card holding more than the current budget allows (a smaller card, a changed segment
    // count, or a backlog left by an older build) is trimmed here so free space is reclaimed
    // before recording resumes -- but only a few per boot.
    //
    // main() does not start feeding the watchdog until after every init step, so all of this
    // runs inside the 30 s window with nothing feeding it. Trimming a large backlog took
    // longer than that and reset the device mid-mount, which trimmed a few more and reset
    // again. Bounding it keeps mount short; the remainder drains through normal rotation,
    // which runs on the storage thread where a slow eviction cannot starve the watchdog.
    uint32_t trimmed = 0;
    while (seg_count > seg_max_count && trimmed < SEG_BOOT_TRIM_MAX) {
        uint8_t before = seg_count;
        segment_evict_oldest_locked();
        if (seg_count == before) {
            break;
        }
        trimmed++;
    }
    if (seg_count > seg_max_count) {
        LOG_WRN("%u segments still over budget; rotation will drain the rest", seg_count - seg_max_count);
    }
    return 0;
}

int mount_sd_card(void)
{
    // initialize the sd card enable pin (v2)
    if (gpio_is_ready_dt(&sd_en_gpio_pin)) {
        LOG_INF("SD Enable Pin ready");
    } else {
        LOG_ERR("Error setting up SD Enable Pin");
        return -1;
    }

    if (gpio_pin_configure_dt(&sd_en_gpio_pin, GPIO_OUTPUT_ACTIVE) < 0) {
        LOG_ERR("Error setting up SD Pin");
        return -1;
    }
    sd_enabled = true;

    // initialize the sd card
    const char *disk_pdrv = "SD";
    int err = disk_access_init(disk_pdrv);
    LOG_INF("disk_access_init: %d\n", err);
    if (err) { // reattempt
        k_msleep(1000);
        err = disk_access_init(disk_pdrv);
        if (err) {
            LOG_ERR("disk_access_init failed");
            return -1;
        }
    }

    mount_point.mnt_point = "/SD:";
    // Never let a failed mount reformat the card. CONFIG_FS_FATFS_MOUNT_MKFS is enabled, so
    // without this flag a card whose FAT does not parse is silently erased on the next boot.
    mount_point.flags = FS_MOUNT_FLAG_NO_FORMAT;
    int res = fs_mount(&mount_point);
    if (res == FR_OK) {
        LOG_INF("SD card mounted successfully");
    } else {
        LOG_ERR("f_mount failed: %d - card left untouched, format it manually if this persists", res);
        return -1;
    }

    res = fs_mkdir("/SD:/audio");
    if (res == 0) {
        LOG_INF("audio directory created");
    } else if (res == -EEXIST) {
        LOG_INF("audio directory already exists");
    } else {
        LOG_ERR("audio directory creation failed: %d", res);
        return -1;
    }

    // Recording must resume where the last power cycle left off, so segments are enumerated
    // rather than recreated; the highest-numbered one is reopened for append.
    k_mutex_lock(&sd_mutex, K_FOREVER);
    res = segment_scan_locked();
    k_mutex_unlock(&sd_mutex);
    if (res) {
        LOG_ERR("failed to scan segments: %d", res);
        return -1;
    }

    struct fs_dirent info_file_entry; // check if the info file exists. if not, generate new info file
    const char *info_path = "/SD:/info.txt";
    res = fs_stat(info_path, &info_file_entry);
    if (res) {
        res = create_file("info.txt");
        save_offset(0);
        LOG_INF("result of info.txt creation: %d ", res);
    }

    k_mutex_lock(&sd_mutex, K_FOREVER);
    // Default the read path at the newest segment; the host retargets it per request.
    seg_audio_path(read_buffer, sizeof(read_buffer), seg_newest);
    res = storage_open_locked();
    if (res == 0) {
        file_num_array[0] = audio_file_size;
    }
    k_mutex_unlock(&sd_mutex);
    if (res) {
        return -1;
    }

    return 0;
}

// Ring accessors. file_num on the wire is 1-based and ordered oldest-first, so that deleting a
// segment shifts the remaining numbers down by one, which is what the app already expects.
uint8_t storage_segment_count(void)
{
    k_mutex_lock(&sd_mutex, K_FOREVER);
    uint8_t n = seg_count;
    k_mutex_unlock(&sd_mutex);
    return n;
}

uint32_t storage_segment_oldest_seq(void)
{
    return seg_oldest;
}

uint32_t storage_segment_newest_seq(void)
{
    return seg_newest;
}

uint32_t storage_segment_target_bytes(void)
{
    return seg_bytes;
}

uint32_t storage_segment_max_count(void)
{
    return seg_max_count;
}

void storage_ring_stats(uint32_t *evictions, int32_t *last_evict_err, uint32_t *sync_errors)
{
    k_mutex_lock(&sd_mutex, K_FOREVER);
    if (evictions) {
        *evictions = ring_evictions;
    }
    if (last_evict_err) {
        *last_evict_err = ring_last_evict_err;
    }
    if (sync_errors) {
        *sync_errors = sync_error_count;
    }
    k_mutex_unlock(&sd_mutex);
}

void storage_io_stats(uint32_t *open_failures,
                      int32_t *last_open_err,
                      uint32_t *write_failures,
                      int32_t *last_write_err)
{
    if (open_failures) {
        *open_failures = io_open_failures;
    }
    if (last_open_err) {
        *last_open_err = io_last_open_err;
    }
    if (write_failures) {
        *write_failures = io_write_failures;
    }
    if (last_write_err) {
        *last_write_err = io_last_write_err;
    }
}

static uint32_t seg_seq_for_num_locked(uint8_t num)
{
    if (num == 0 || num > seg_count) {
        return 0;
    }
    return seg_seq[num - 1];
}

uint32_t get_file_size(uint8_t num)
{
    k_mutex_lock(&sd_mutex, K_FOREVER);
    uint32_t seq = seg_seq_for_num_locked(num);
    if (seq == 0) {
        k_mutex_unlock(&sd_mutex);
        return 0;
    }

    // For the segment being appended to, the live counter is authoritative: fs_stat only sees
    // what has already been flushed, so it under-reports by up to one batch.
    if (seq == seg_newest && audio_file_open) {
        uint32_t size = audio_file_size;
        k_mutex_unlock(&sd_mutex);
        return size;
    }

    char path[MAX_PATH_LENGTH];
    seg_audio_path(path, sizeof(path), seq);
    struct fs_dirent entry;
    int res = fs_stat(path, &entry);
    k_mutex_unlock(&sd_mutex);
    if (res) {
        LOG_ERR("invalid file in get file size: %d", res);
        return 0;
    }
    return (uint32_t) entry.size;
}

int move_read_pointer(uint8_t num)
{
    k_mutex_lock(&sd_mutex, K_FOREVER);
    uint32_t seq = seg_seq_for_num_locked(num);
    if (seq == 0) {
        k_mutex_unlock(&sd_mutex);
        LOG_ERR("invalid segment %u in move read ptr", num);
        return -1;
    }
    read_cache_invalidate_locked();
    seg_audio_path(read_buffer, sizeof(read_buffer), seq);
    read_target_is_newest = (seq == seg_newest);
    k_mutex_unlock(&sd_mutex);
    return 0;
}

int move_write_pointer(uint8_t num)
{
    k_mutex_lock(&sd_mutex, K_FOREVER);
    uint32_t seq = seg_seq_for_num_locked(num);
    if (seq == 0) {
        k_mutex_unlock(&sd_mutex);
        LOG_ERR("invalid segment %u in move write pointer", num);
        return -1;
    }
    seg_audio_path(write_buffer, sizeof(write_buffer), seq);
    k_mutex_unlock(&sd_mutex);
    return 0;
}

int create_file(const char *file_path)
{
    int ret = 0;
    k_mutex_lock(&sd_mutex, K_FOREVER);
    snprintf(current_full_path, sizeof(current_full_path), "%s%s", disk_mount_pt, file_path);
    struct fs_file_t data_file;
    fs_file_t_init(&data_file);
    ret = fs_open(&data_file, current_full_path, FS_O_WRITE | FS_O_CREATE);
    if (ret) {
        LOG_ERR("File creation failed %d", ret);
        k_mutex_unlock(&sd_mutex);
        return -2;
    }
    fs_close(&data_file);
    k_mutex_unlock(&sd_mutex);
    return 0;
}

static void read_cache_invalidate_locked(void)
{
    read_cache_start = 0;
    read_cache_len = 0;
    if (read_file_open) {
        fs_close(&read_file);
        read_file_open = false;
    }
}

// Refill the cache so that it covers `offset`. Returns 0 on success.
static int read_cache_fill_locked(uint32_t offset)
{
    // Only commit when this refill actually reaches into audio that is still in RAM. A
    // download refills far faster than the batch fills, so flushing unconditionally wrote a
    // near-empty batch every time -- reintroducing the small random writes that the batch
    // exists to avoid, and slowing the transfer to a crawl. Sealed segments can never hold
    // buffered audio, and audio_file_size describes only the newest one, so checking it for
    // any other target would flush for no reason.
    if (read_target_is_newest && offset + READ_CACHE_SIZE > audio_file_size - write_batch_len) {
        write_batch_flush_locked();
    }

    if (!read_file_open) {
        fs_file_t_init(&read_file);
        int rc = fs_open(&read_file, read_buffer, FS_O_READ);
        if (rc) {
            LOG_ERR("read: open failed %d", rc);
            return rc;
        }
        read_file_open = true;
    }

    int rc = fs_seek(&read_file, offset, FS_SEEK_SET);
    if (rc) {
        LOG_ERR("read: seek to %u failed %d", offset, rc);
        read_cache_invalidate_locked();
        return rc;
    }

    rc = fs_read(&read_file, read_cache, READ_CACHE_SIZE);
    if (rc < 0) {
        LOG_ERR("read: failed %d", rc);
        read_cache_invalidate_locked();
        return rc;
    }

    read_cache_start = offset;
    read_cache_len = (uint32_t) rc;
    return 0;
}

int read_audio_data(uint8_t *buf, int amount, int offset)
{
    if (amount <= 0 || offset < 0) {
        return -EINVAL;
    }

    k_mutex_lock(&sd_mutex, K_FOREVER);

    uint32_t want = (uint32_t) amount;
    uint32_t off = (uint32_t) offset;

    bool cached = read_cache_len > 0 && off >= read_cache_start && off < read_cache_start + read_cache_len;
    if (!cached) {
        int rc = read_cache_fill_locked(off);
        if (rc) {
            k_mutex_unlock(&sd_mutex);
            return rc;
        }
        if (read_cache_len == 0) {
            k_mutex_unlock(&sd_mutex);
            return 0; // end of file
        }
    }

    uint32_t avail = read_cache_start + read_cache_len - off;
    uint32_t give = MIN(want, avail);
    memcpy(buf, read_cache + (off - read_cache_start), give);

    k_mutex_unlock(&sd_mutex);
    return (int) give;
}

void storage_read_close(void)
{
    k_mutex_lock(&sd_mutex, K_FOREVER);
    read_cache_invalidate_locked();
    k_mutex_unlock(&sd_mutex);
}

int storage_select_read_target(uint8_t num, int target)
{
    k_mutex_lock(&sd_mutex, K_FOREVER);
    uint32_t seq = seg_seq_for_num_locked(num);
    if (seq == 0) {
        k_mutex_unlock(&sd_mutex);
        return -ENOENT;
    }
    // Switching files must drop the cache: it is keyed only by byte offset and would
    // otherwise serve audio bytes as index records, or one segment's bytes as another's.
    read_cache_invalidate_locked();
    if (target == STORAGE_READ_INDEX) {
        seg_index_path(read_buffer, sizeof(read_buffer), seq);
        read_target_is_newest = false; // the index is written and closed per record
    } else {
        seg_audio_path(read_buffer, sizeof(read_buffer), seq);
        read_target_is_newest = (seq == seg_newest);
    }
    k_mutex_unlock(&sd_mutex);
    return 0;
}

uint32_t index_get_size_for(uint8_t num)
{
    k_mutex_lock(&sd_mutex, K_FOREVER);
    uint32_t seq = seg_seq_for_num_locked(num);
    if (seq == 0) {
        k_mutex_unlock(&sd_mutex);
        return 0;
    }
    char path[MAX_PATH_LENGTH];
    seg_index_path(path, sizeof(path), seq);
    struct fs_dirent entry;
    int rc = fs_stat(path, &entry);
    k_mutex_unlock(&sd_mutex);
    return rc ? 0 : (uint32_t) entry.size;
}

int write_to_file(uint8_t *data, uint32_t length)
{
    k_mutex_lock(&sd_mutex, K_FOREVER);

    int rc = storage_open_locked();
    if (rc) {
        k_mutex_unlock(&sd_mutex);
        return rc;
    }

    const uint8_t *src = data;
    uint32_t remaining = length;
    while (remaining > 0) {
        uint32_t n = MIN(WRITE_BATCH_SIZE - write_batch_len, remaining);
        memcpy(write_batch + write_batch_len, src, n);
        write_batch_len += n;
        audio_file_size += n;
        src += n;
        remaining -= n;

        if (write_batch_len == WRITE_BATCH_SIZE) {
            rc = write_batch_flush_locked();
            if (rc) {
                k_mutex_unlock(&sd_mutex);
                return rc;
            }
        }
    }

    // Stamp the offset that has just been reached, so the host can map bytes back to time.
    if (rtc_get_uptime_s() >= index_next_uptime_s) {
        index_append_locked();
    }

    k_mutex_unlock(&sd_mutex);
    return (int) length;
}

// we should clear instead of delete since we lose fifo structure
// Host-initiated delete of one segment, normally after it has been synced. Deleting the
// segment currently being appended to is allowed but means starting a fresh one, since a
// recorder that has nowhere to write is worse than a small gap.
int clear_audio_file(uint8_t num)
{
    k_mutex_lock(&sd_mutex, K_FOREVER);

    if (num == 0 || num > seg_count) {
        k_mutex_unlock(&sd_mutex);
        LOG_ERR("delete: no segment %u", num);
        return -1;
    }

    bool was_newest = (seg_seq[num - 1] == seg_newest);
    int res = segment_drop_at_locked(num - 1);
    if (res) {
        // segment_drop_at_locked already closed the append handle if it was the target.
        storage_open_locked();
        k_mutex_unlock(&sd_mutex);
        return -1;
    }

    if (was_newest) {
        // Nothing is open for append now. Start the next segment rather than reusing the
        // number, so any host still holding the old sequence cannot confuse the two.
        if (seg_count >= SEG_MAX_COUNT || seg_newest >= SEG_SEQ_MAX) {
            k_mutex_unlock(&sd_mutex);
            LOG_ERR("delete: cannot open a replacement segment");
            return -1;
        }
        seg_seq[seg_count++] = seg_newest + 1u;
        seg_refresh_bounds();
        audio_file_size = 0;
        res = storage_open_locked();
        if (res) {
            k_mutex_unlock(&sd_mutex);
            LOG_ERR("delete: reopening after clear failed: %d", res);
            return -1;
        }
        index_next_uptime_s = 0;
        index_append_locked();
    }

    file_num_array[0] = audio_file_size;
    k_mutex_unlock(&sd_mutex);
    return 0;
}

int delete_audio_file(uint8_t num)
{
    k_mutex_lock(&sd_mutex, K_FOREVER);
    int res = (num == 0 || num > seg_count) ? -ENOENT : segment_drop_at_locked(num - 1);
    k_mutex_unlock(&sd_mutex);
    return res ? -1 : 0;
}

// The nuclear option: drop every segment and start again from a single empty one.
int clear_audio_directory()
{
    k_mutex_lock(&sd_mutex, K_FOREVER);

    write_batch_len = 0;
    storage_close_locked();
    read_cache_invalidate_locked();

    while (seg_count > 0) {
        uint8_t before = seg_count;
        if (segment_drop_at_locked(0) != 0 && seg_count == before) {
            break;
        }
    }

    if (seg_count != 0) {
        k_mutex_unlock(&sd_mutex);
        LOG_ERR("nuke: could not remove every segment");
        return -1;
    }

    seg_seq[0] = (seg_newest < SEG_SEQ_MAX) ? seg_newest + 1u : 1u;
    seg_count = 1;
    seg_refresh_bounds();
    audio_file_size = 0;

    int res = storage_open_locked();
    if (res) {
        k_mutex_unlock(&sd_mutex);
        LOG_ERR("nuke: failed to open fresh segment: %d", res);
        return -1;
    }
    index_next_uptime_s = 0;
    index_append_locked();

    file_num_array[0] = 0;
    k_mutex_unlock(&sd_mutex);
    LOG_INF("cleared all segments, recording into %u", seg_newest);
    return 0;
}

int save_offset(uint32_t offset)
{
    uint8_t buf[4] = {offset & 0xFF, (offset >> 8) & 0xFF, (offset >> 16) & 0xFF, (offset >> 24) & 0xFF};

    k_mutex_lock(&sd_mutex, K_FOREVER);
    struct fs_file_t write_file;
    fs_file_t_init(&write_file);
    int res = fs_open(&write_file, "/SD:/info.txt", FS_O_WRITE | FS_O_CREATE);
    if (res) {
        LOG_ERR("error opening file %d", res);
        k_mutex_unlock(&sd_mutex);
        return -1;
    }
    res = fs_write(&write_file, &buf, 4);
    if (res < 0) {
        LOG_ERR("error writing file %d", res);
        fs_close(&write_file);
        k_mutex_unlock(&sd_mutex);
        return -1;
    }
    fs_close(&write_file);
    k_mutex_unlock(&sd_mutex);
    return 0;
}

int get_offset()
{
    uint8_t buf[4] = {0};
    k_mutex_lock(&sd_mutex, K_FOREVER);
    struct fs_file_t read_file;
    fs_file_t_init(&read_file);
    int rc = fs_open(&read_file, "/SD:/info.txt", FS_O_READ);
    if (rc < 0) {
        LOG_ERR("error opening file %d", rc);
        k_mutex_unlock(&sd_mutex);
        return -1;
    }
    rc = fs_read(&read_file, &buf, 4);
    fs_close(&read_file);
    k_mutex_unlock(&sd_mutex);
    if (rc < 0) {
        LOG_ERR("error reading file %d", rc);
        return -1;
    }

    uint32_t offset =
        (uint32_t) buf[0] | ((uint32_t) buf[1] << 8) | ((uint32_t) buf[2] << 16) | ((uint32_t) buf[3] << 24);
    return (int) offset;
}

void sd_off()
{
    // Commit and release the append handle before the bus goes away, otherwise the buffered
    // tail of the recording is lost.
    k_mutex_lock(&sd_mutex, K_FOREVER);
    write_batch_flush_locked();
    storage_close_locked();
    read_cache_invalidate_locked();
    k_mutex_unlock(&sd_mutex);

    // Suspend SPI peripheral to save power
    const struct device *spi_dev = DEVICE_DT_GET(DT_NODELABEL(spi2));
    if (device_is_ready(spi_dev)) {
        pm_device_action_run(spi_dev, PM_DEVICE_ACTION_SUSPEND);
    }
    gpio_pin_configure(DEVICE_DT_GET(DT_NODELABEL(gpio1)), 15, GPIO_DISCONNECTED); // MOSI
    gpio_pin_configure(DEVICE_DT_GET(DT_NODELABEL(gpio1)), 14, GPIO_DISCONNECTED); // MISO
    gpio_pin_configure(DEVICE_DT_GET(DT_NODELABEL(gpio1)), 13, GPIO_DISCONNECTED); // SCK
    gpio_pin_configure(DEVICE_DT_GET(DT_NODELABEL(gpio0)), 2, GPIO_DISCONNECTED);  // CS
    gpio_pin_set_dt(&sd_en_gpio_pin, 0);

    sd_enabled = false;
}

void sd_on()
{
    gpio_pin_set_dt(&sd_en_gpio_pin, 1);
    gpio_pin_configure(DEVICE_DT_GET(DT_NODELABEL(gpio1)), 15, GPIO_OUTPUT);     // MOSI
    gpio_pin_configure(DEVICE_DT_GET(DT_NODELABEL(gpio1)), 14, GPIO_INPUT);      // MISO
    gpio_pin_configure(DEVICE_DT_GET(DT_NODELABEL(gpio1)), 13, GPIO_OUTPUT);     // SCK
    gpio_pin_configure(DEVICE_DT_GET(DT_NODELABEL(gpio0)), 2, GPIO_OUTPUT_HIGH); // CS
    const struct device *spi_dev = DEVICE_DT_GET(DT_NODELABEL(spi2));
    if (device_is_ready(spi_dev)) {
        pm_device_action_run(spi_dev, PM_DEVICE_ACTION_RESUME);
    }
    sd_enabled = true;

    k_mutex_lock(&sd_mutex, K_FOREVER);
    storage_open_locked();
    k_mutex_unlock(&sd_mutex);
}

bool is_sd_on()
{
    return sd_enabled;
}

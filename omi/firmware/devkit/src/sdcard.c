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

struct gpio_dt_spec sd_en_gpio_pin = {.port = DEVICE_DT_GET(DT_NODELABEL(gpio0)),
                                      .pin = 19,
                                      .dt_flags = GPIO_INT_DISABLE};

uint8_t file_count = 0;

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

// Stay clear of the FAT32 4 GiB per-file ceiling; roughly 8 days of continuous audio.
#define MAX_RECORDING_BYTES 0xF0000000U

#define AUDIO_FILE_PATH "/SD:/audio/a01.txt"

// Timestamps live beside the recording rather than inside it, so the audio stream stays a
// plain sequence of Opus frames that existing decoders can still read.
#define INDEX_FILE_PATH "/SD:/audio/a01.idx"
#define BOOT_ID_PATH "/SD:/boot.bin"

#define INDEX_RECORD_SIZE 16
#define INDEX_INTERVAL_S 30

static uint32_t index_next_uptime_s = 0;

static void storage_close_locked(void);

static int storage_open_locked(void)
{
    if (audio_file_open) {
        return 0;
    }

    fs_file_t_init(&audio_file);
    int rc = fs_open(&audio_file, AUDIO_FILE_PATH, FS_O_WRITE | FS_O_APPEND | FS_O_CREATE);
    if (rc) {
        LOG_ERR("Failed to open %s for append: %d", AUDIO_FILE_PATH, rc);
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

    ssize_t written = fs_write(&audio_file, write_batch, write_batch_len);
    if (written < 0) {
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

    struct fs_file_t f;
    fs_file_t_init(&f);
    int rc = fs_open(&f, INDEX_FILE_PATH, FS_O_WRITE | FS_O_APPEND | FS_O_CREATE);
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

void storage_index_mark(bool force)
{
    k_mutex_lock(&sd_mutex, K_FOREVER);
    if (force || rtc_get_uptime_s() >= index_next_uptime_s) {
        index_append_locked();
    }
    k_mutex_unlock(&sd_mutex);
}

uint32_t index_get_size(void)
{
    k_mutex_lock(&sd_mutex, K_FOREVER);
    struct fs_dirent entry;
    int rc = fs_stat(INDEX_FILE_PATH, &entry);
    k_mutex_unlock(&sd_mutex);
    return rc ? 0 : (uint32_t) entry.size;
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

    file_count = 1;

    // Recording must resume where the last power cycle left off, so the audio file is only
    // created when it is genuinely absent.
    struct fs_dirent audio_entry;
    res = fs_stat(AUDIO_FILE_PATH, &audio_entry);
    if (res == -ENOENT) {
        LOG_INF("no audio file yet, creating %s", AUDIO_FILE_PATH);
        res = create_file("audio/a01.txt");
        if (res) {
            LOG_ERR("failed to create audio file: %d", res);
            return -1;
        }
    } else if (res) {
        LOG_ERR("failed to stat audio file: %d", res);
        return -1;
    } else {
        LOG_INF("existing recording found: %u bytes", (uint32_t) audio_entry.size);
    }

    res = move_write_pointer(file_count);
    if (res) {
        LOG_ERR("error while moving the write pointer");
        return -1;
    }

    res = move_read_pointer(file_count);
    if (res) {
        LOG_ERR("error while moving the reader pointer");
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

uint32_t get_file_size(uint8_t num)
{
    // The live byte count is authoritative while the append handle is open: fs_stat only sees
    // what has been synced, and it races with the recording thread over current_full_path.
    if (num == 1) {
        k_mutex_lock(&sd_mutex, K_FOREVER);
        bool open = audio_file_open;
        uint32_t size = audio_file_size;
        k_mutex_unlock(&sd_mutex);
        if (open) {
            return size;
        }
    }

    k_mutex_lock(&sd_mutex, K_FOREVER);
    char *ptr = generate_new_audio_header(num);
    if (ptr == NULL) {
        k_mutex_unlock(&sd_mutex);
        return 0;
    }
    snprintf(current_full_path, sizeof(current_full_path), "%s%s", disk_mount_pt, ptr);
    k_free(ptr);
    struct fs_dirent entry;
    int res = fs_stat(current_full_path, &entry);
    k_mutex_unlock(&sd_mutex);
    if (res) {
        LOG_ERR("invalid file in get file size: %d", res);
        return 0;
    }
    return (uint32_t) entry.size;
}

int move_read_pointer(uint8_t num)
{
    char *read_ptr = generate_new_audio_header(num);
    snprintf(read_buffer, sizeof(read_buffer), "%s%s", disk_mount_pt, read_ptr);
    k_free(read_ptr);
    struct fs_dirent entry;
    int res = fs_stat(read_buffer, &entry);
    if (res) {
        LOG_ERR("invalid file in move read ptr\n");
        return -1;
    }
    return 0;
}

int move_write_pointer(uint8_t num)
{
    char *write_ptr = generate_new_audio_header(num);
    snprintf(write_buffer, sizeof(write_buffer), "%s%s", disk_mount_pt, write_ptr);
    k_free(write_ptr);
    struct fs_dirent entry;
    int res = fs_stat(write_buffer, &entry);
    if (res) {
        LOG_ERR("invalid file in move write pointer\n");
        return -1;
    }
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
    // exists to avoid, and slowing the transfer to a crawl.
    if (offset + READ_CACHE_SIZE > audio_file_size - write_batch_len) {
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

void storage_select_read_target(int target)
{
    k_mutex_lock(&sd_mutex, K_FOREVER);
    // Switching files must drop the cache: it is keyed only by byte offset and would
    // otherwise serve audio bytes as index records.
    read_cache_invalidate_locked();
    if (target == STORAGE_READ_INDEX) {
        snprintf(read_buffer, sizeof(read_buffer), "%s", INDEX_FILE_PATH);
    } else {
        snprintf(read_buffer, sizeof(read_buffer), "%s", AUDIO_FILE_PATH);
    }
    k_mutex_unlock(&sd_mutex);
}

int write_to_file(uint8_t *data, uint32_t length)
{
    k_mutex_lock(&sd_mutex, K_FOREVER);

    int rc = storage_open_locked();
    if (rc) {
        k_mutex_unlock(&sd_mutex);
        return rc;
    }

    if (audio_file_size + length > MAX_RECORDING_BYTES) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            LOG_ERR("recording reached %u bytes, refusing further writes until it is cleared", audio_file_size);
        }
        k_mutex_unlock(&sd_mutex);
        return -ENOSPC;
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

int initialize_audio_file(uint8_t num)
{
    char *header = generate_new_audio_header(num);
    if (header == NULL) {
        return -1;
    }
    int rc = create_file(header);
    k_free(header);
    return rc;
}

char *generate_new_audio_header(uint8_t num)
{
    if (num > 99)
        return NULL;
    char *ptr_ = k_malloc(14);
    ptr_[0] = 'a';
    ptr_[1] = 'u';
    ptr_[2] = 'd';
    ptr_[3] = 'i';
    ptr_[4] = 'o';
    ptr_[5] = '/';
    ptr_[6] = 'a';
    ptr_[7] = 48 + (num / 10);
    ptr_[8] = 48 + (num % 10);
    ptr_[9] = '.';
    ptr_[10] = 't';
    ptr_[11] = 'x';
    ptr_[12] = 't';
    ptr_[13] = '\0';

    return ptr_;
}

int get_file_contents(struct fs_dir_t *zdp, struct fs_dirent *entry)
{
    if (zdp->mp->fs->readdir(zdp, entry)) {
        return -1;
    }
    if (entry->name[0] == 0) {
        return 0;
    }
    int count = 0;
    file_num_array[count] = entry->size;
    LOG_INF("file numarray %d %d ", count, file_num_array[count]);
    LOG_INF("file name is %s ", entry->name);
    count++;
    while (zdp->mp->fs->readdir(zdp, entry) == 0) {
        if (entry->name[0] == 0) {
            break;
        }
        file_num_array[count] = entry->size;
        LOG_INF("file numarray %d %d ", count, file_num_array[count]);
        LOG_INF("file name is %s ", entry->name);
        count++;
    }
    return count;
}
// we should clear instead of delete since we lose fifo structure
int clear_audio_file(uint8_t num)
{
    k_mutex_lock(&sd_mutex, K_FOREVER);

    // The append handle points at the inode about to be unlinked; it has to be dropped and
    // re-established or recording silently stops until the next reboot. The read cache would
    // otherwise keep serving bytes from the deleted file, and the batch holds audio that
    // belongs to the file being discarded.
    write_batch_len = 0;
    storage_close_locked();
    read_cache_invalidate_locked();

    char *clear_header = generate_new_audio_header(num);
    if (clear_header == NULL) {
        k_mutex_unlock(&sd_mutex);
        return -1;
    }
    snprintf(current_full_path, sizeof(current_full_path), "%s%s", disk_mount_pt, clear_header);
    k_free(clear_header);

    int res = fs_unlink(current_full_path);
    if (res) {
        LOG_ERR("error deleting file: %d", res);
        storage_open_locked();
        k_mutex_unlock(&sd_mutex);
        return -1;
    }

    res = initialize_audio_file(num);
    if (res) {
        LOG_ERR("error creating file: %d", res);
        k_mutex_unlock(&sd_mutex);
        return -1;
    }

    audio_file_size = 0;
    res = storage_open_locked();
    if (res) {
        LOG_ERR("error reopening audio file after clear: %d", res);
        k_mutex_unlock(&sd_mutex);
        return -1;
    }

    // The index describes offsets into the file that was just discarded, so it has to go too
    // or every timestamp would point at the wrong audio.
    fs_unlink(INDEX_FILE_PATH);
    index_next_uptime_s = 0;
    index_append_locked();

    file_num_array[0] = 0;
    k_mutex_unlock(&sd_mutex);
    return 0;
}

int delete_audio_file(uint8_t num)
{
    char *ptr = generate_new_audio_header(num);
    snprintf(current_full_path, sizeof(current_full_path), "%s%s", disk_mount_pt, ptr);
    k_free(ptr);
    int res = fs_unlink(current_full_path);
    if (res) {
        LOG_PRINTK("error deleting file in delete\n");
        return -1;
    }

    return 0;
}
// the nuclear option.
int clear_audio_directory()
{
    if (file_count == 1) {
        // Single-file layout: wiping the directory reduces to clearing the one audio file,
        // which also rebuilds the append handle.
        return clear_audio_file(1);
    }
    // check if all files are zero
    //  char* path_ = "/SD:/audio";
    //  clear_audio_file(file_count);
    int res = 0;
    for (uint8_t i = file_count; i > 0; i--) {
        res = delete_audio_file(i);
        k_msleep(10);
        if (res) {
            LOG_PRINTK("error on %d\n", i);
            return -1;
        }
    }
    res = fs_unlink("/SD:/audio");
    if (res) {
        LOG_ERR("error deleting file");
        return -1;
    }
    res = fs_mkdir("/SD:/audio");
    if (res) {
        LOG_ERR("failed to make directory");
        return -1;
    }
    res = create_file("audio/a01.txt");
    if (res) {
        LOG_ERR("failed to make new file in directory files");
        return -1;
    }
    LOG_ERR("done with clearing");

    file_count = 1;
    move_write_pointer(1);
    return 0;
    // if files are cleared, then directory is oked for destrcution.
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

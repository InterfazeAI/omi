#ifndef SDCARD_H
#define SDCARD_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Most functions here reach FatFs, which needs far more stack than a Bluetooth callback has.
 * CONFIG_BT_RX_STACK_SIZE is 1024, and a GATT handler that calls into the filesystem overflows
 * it and faults the device -- a silent reboot that looks like a dropped connection from the
 * host. Two separate bugs shipped this way (DEBUGGING.md trap 7).
 *
 * Call these from the storage or pusher thread. From a GATT handler, use only the accessors
 * documented below as filesystem-free, or latch the work for the storage thread the way
 * storage_index_mark() does.
 */

/**
 * @brief Mount the SD Card and reopen the newest recording segment for append
 *
 * Recording is a ring of fixed-size segment files (`/SD:/audio/aNNNNN.txt`), sized at mount
 * from the card's free space. When the ring is full the oldest segment is unlinked, so
 * recording continues indefinitely rather than stopping. Existing segments are enumerated
 * rather than recreated, so a recording survives power cycles; the card is never reformatted
 * automatically.
 *
 * @return 0 if successful, negative errno code if error
 */
int mount_sd_card(void);

/**
 * @brief Ring geometry, for reporting to a host.
 *
 * Segments are addressed on the wire by a 1-based number, ordered oldest first, so deleting
 * one shifts the numbers above it down by one.
 */
uint8_t storage_segment_count(void);
uint32_t storage_segment_oldest_seq(void);
uint32_t storage_segment_newest_seq(void);
uint32_t storage_segment_target_bytes(void);
uint32_t storage_segment_max_count(void);

/**
 * @brief Ring health counters, for reporting to a host.
 *
 * Eviction happens inside the write path, where a failure can only be logged — and the log
 * thread runs below the codec, so it may never be emitted. These make a ring that has stopped
 * evicting, or a card that is genuinely failing its syncs, visible over BLE. Any argument may
 * be NULL.
 */
void storage_ring_stats(uint32_t *evictions, int32_t *last_evict_err, uint32_t *sync_errors);

/**
 * @brief Commit buffered audio to the card
 *
 * Writes are synced periodically; call this to force a commit, e.g. before a BLE sync so the
 * reader observes everything captured so far.
 *
 * @return 0 if successful, negative errno code if error
 */
int storage_flush(void);

/**
 * @brief Current length of the recording in bytes
 *
 * Tracked live against the open append handle, so it stays correct between syncs.
 */
uint32_t storage_get_size(void);

/**
 * @brief Release the read handle and cache used while serving a sync
 *
 * Call when a transfer finishes or is aborted. Reads re-open lazily.
 */
void storage_read_close(void);

#define STORAGE_READ_AUDIO 0
#define STORAGE_READ_INDEX 1

/**
 * @brief Choose which file subsequent read_audio_data() calls serve.
 *
 * Selects either the audio or the timestamp index of segment @p num, so the index can be
 * handed to a client over the same transfer path as the audio.
 *
 * @return 0 if successful, -ENOENT if there is no such segment.
 */
int storage_select_read_target(uint8_t num, int target);

/**
 * @brief Size of segment @p num's timestamp index in bytes, 0 if absent.
 */
uint32_t index_get_size_for(uint8_t num);

/**
 * @brief Request a timestamp record mapping the current audio offset to the current time.
 *
 * Records the request and returns; storage_index_service() does the write. Safe to call from
 * any context, including a GATT handler, which is why it is deferred — see the filesystem
 * warning below.
 *
 * @param force Write on the next service call rather than waiting for the next interval.
 */
void storage_index_mark(bool force);

/**
 * @brief Perform any pending storage_index_mark(). Call only from the storage thread.
 */
void storage_index_service(void);

/**
 * @brief Load/store the power-on counter kept on the card.
 *
 * @return 0 on success, negative errno otherwise.
 */
int storage_load_boot_id(uint32_t *out);
int storage_save_boot_id(uint32_t id);

/**
 * @brief Create a file
 *
 * Creates a file at the given path
 *
 * @return 0 if successful, negative errno code if error
 */
int create_file(const char *file_path);

/**
 * @brief Write to the current audio file specified by the write pointer
 *
 *
 *
 * @return number of bytes written
 */
int write_to_file(uint8_t *data, uint32_t length);

/**
 * @brief Read from the current audio file specified by the read pointer
 *
 *
 *
 * @return number of bytes read
 */
int read_audio_data(uint8_t *buf, int amount, int offset);
/**
 * @brief Get the size of the specified audio file number
 *
 *
 *
 * @return size of the file in bytes
 */
uint32_t get_file_size(uint8_t num);

/**
 * @brief Move the read pointer to the specified audio file position
 *
 *
 *
 * @return 0 if successful, negative errno code if error
 */
int move_read_pointer(uint8_t num);

/**
 * @brief Move the write pointer to the specified audio file position
 *
 *
 *
 * @return 0 if successful, negative errno code if error
 */
int move_write_pointer(uint8_t num);

/**
 * @brief Delete segment @p num, normally after a host has synced it.
 *
 * Deleting the segment currently being recorded into is allowed: a fresh one is opened
 * immediately, because a recorder with nowhere to write is worse than a short gap.
 *
 * @return 0 if successful, negative errno code if error
 */
int clear_audio_file(uint8_t num);

/**
 * @brief Delete segment @p num without opening a replacement.
 */
int delete_audio_file(uint8_t num);

/**
 * @brief Drop every segment and start recording into a single fresh one.
 *
 * @return 0 if successful, negative errno code if error
 */
int clear_audio_directory();

int save_offset(uint32_t offset);
int get_offset();

void sd_on();
void sd_off();

bool is_sd_on();
#endif

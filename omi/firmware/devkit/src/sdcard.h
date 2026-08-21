#ifndef SDCARD_H
#define SDCARD_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Mount the SD Card and open the recording for append
 *
 * Mounts the SD Card and opens the audio file, creating it only when absent so that a
 * recording survives power cycles. The card is never reformatted automatically.
 *
 * @return 0 if successful, negative errno code if error
 */
int mount_sd_card(void);

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
 * Used to hand the timestamp index to a client over the same transfer path as the audio.
 */
void storage_select_read_target(int target);

/**
 * @brief Size of the timestamp index in bytes, 0 if absent.
 */
uint32_t index_get_size(void);

/**
 * @brief Append a timestamp record mapping the current audio offset to the current time.
 *
 * @param force Write immediately rather than waiting for the next interval.
 */
void storage_index_mark(bool force);

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
// private
char *generate_new_audio_header(uint8_t num);

/**
 * @brief Initialize an audio file of number 1
 *
 * Initializes an audio file. It will be called a nn.txt, where nn is the number of the file.
 *  example: initialize_audio_file(1) will create a file called a01.txt
 * @return 0 if successful, negative errno code if error
 */
int initialize_audio_file(uint8_t num);

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
 * @brief Clear the specified audio file
 *
 *
 *
 * @return 0 if successful, negative errno code if error
 */
int clear_audio_file(uint8_t num);

/**
 * @brief Clear the audio directory.
 *
 * This deletes all audio files and leaves the audio directory with only one file left, a01.txt.
 * This automatically moves the read and write pointers to a01.txt.
 * @return 0 if successful, negative errno code if error
 */
int clear_audio_directory();

int save_offset(uint32_t offset);
int get_offset();

void sd_on();
void sd_off();

bool is_sd_on();
#endif

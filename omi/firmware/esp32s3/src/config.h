#ifndef CONFIG_H
#define CONFIG_H

// BOARD_HAS_PSRAM comes from platformio.ini and CONFIG_ARDUHAL_ESP_LOG from the
// precompiled ESP-IDF; defining either here only produces redefinition warnings.

// =============================================================================
// DEVICE CONFIGURATION
// =============================================================================
#define BLE_DEVICE_NAME "Omi"

// Firmware revision reported over DIS (0x2A26). MUST stay purely numeric and
// >= 3.0.20: the app parses each dotted part with int.tryParse and a 0 fallback,
// so a suffix like "3.0.21-esp32" silently parses as 3.0.0 and routes to the
// legacy single-file storage protocol instead of the ring protocol.
#define FIRMWARE_VERSION_STRING "3.0.21"
#define HARDWARE_REVISION "XIAO-ESP32S3-Sense-v1.0"
#define MANUFACTURER_NAME "Based Hardware"

// =============================================================================
// POWER MANAGEMENT
// =============================================================================
// Power is a hardware slide switch in the battery line (as on DevKit1), so there
// is no firmware-visible power control: no button, no user-triggered shutdown,
// and no deep sleep (nothing could wake the device from it).
#define MIN_CPU_FREQ_MHZ 80     // Idle: gate closed and no BLE peer
#define NORMAL_CPU_FREQ_MHZ 160 // Recording / streaming / syncing
#define IDLE_THRESHOLD_MS 45000 // Idle time before dropping to MIN_CPU_FREQ_MHZ

// Battery Configuration - Dual 250mAh @ 3.5V-4.1V under load (500mAh total)
#define BATTERY_MAX_VOLTAGE 4.2f      // 4.2V fully charged (under load)
#define BATTERY_MIN_VOLTAGE 3.2f      // 3.2V empty (under load)
#define BATTERY_CRITICAL_VOLTAGE 3.3f // Emergency shutdown voltage
#define BATTERY_LOW_VOLTAGE 3.4f      // Low battery warning
#define VOLTAGE_DIVIDER_RATIO 6.086f  // Calibrated to match multimeter readings (load-compensated)

// Battery Monitoring
#define BATTERY_REPORT_INTERVAL_MS 90000 // 1.5 minute reporting
#define BATTERY_TASK_INTERVAL_MS 20000   // 20 second internal checks

// =============================================================================
// PIN DEFINITIONS - XIAO ESP32S3 Sense
// =============================================================================
// XIAO silkscreen D0..D10 map to GPIO 1,2,3,4,5,6,43,44,7,8,9. The analog pins
// A0..A5 are the same physical pins as D0..D5, so A0 == GPIO1 and A1 == GPIO2.
//
// The battery divider is on A1/GPIO2. omiGlass/firmware/readme.md claims A0, but
// A0/GPIO1 is what that firmware wired its power button to (config.h:215,
// "GPIO1/A0"), so it cannot also carry the divider. config.h is authoritative;
// the readme is wrong. Removing the button frees GPIO1 for the status LED.
#define BATTERY_ADC_PIN 2 // A1 / GPIO2 - battery voltage divider

// Status LED. NOT the onboard user LED (GPIO21): on the Sense expansion board
// GPIO21 is the SD card chip-select, so driving it as an LED would fight the SD
// bus. External LED on GPIO1, freed by the button removal. Inverted (LOW = on)
// to match the onboard LED wiring convention.
#define STATUS_LED_PIN 1
#define STATUS_LED_ACTIVE_LOW 1

// SD card over SPI on the Sense expansion board. Requires the J3 solder pads to
// be bridged (they are by default from the factory).
#define SD_SCK_PIN 7
#define SD_MISO_PIN 8
#define SD_MOSI_PIN 9
#define SD_CS_PIN 21
#define SD_SPI_FREQ_HZ 20000000

// =============================================================================
// MICROPHONE CONFIGURATION - I2S PDM (XIAO ESP32S3 Sense built-in mic)
// =============================================================================
#define MIC_CLK_PIN 42  // PDM Clock pin (GPIO42)
#define MIC_DATA_PIN 41 // PDM Data pin (GPIO41)

#define MIC_SAMPLE_RATE 16000          // 16kHz sample rate
#define MIC_BUFFER_SAMPLES 1600        // 100ms buffer (16000 * 0.1)
#define MIC_GAIN 2                     // Microphone gain multiplier
#define AUDIO_RING_BUFFER_SAMPLES 8000 // 500ms of audio data

// =============================================================================
// OPUS CODEC CONFIGURATION
// =============================================================================
#define AUDIO_CODEC_ID 21      // opusFS320 (320-sample / 20ms frames)
#define OPUS_FRAME_SAMPLES 320 // 20ms frame @ 16kHz
#define OPUS_FRAME_MS (OPUS_FRAME_SAMPLES * 1000 / MIC_SAMPLE_RATE)
#define OPUS_OUTPUT_MAX_BYTES 160 // Max encoded frame size
#define OPUS_BITRATE 32000        // 32kbps
#define OPUS_COMPLEXITY 3         // Encoding complexity (1-10)
#define OPUS_VBR 1                // Variable bitrate enabled
// DTX collapses in-session silence (word gaps, the hangover tail) to 1-2 byte
// frames while still emitting one frame per 20ms, so the timeline stays exact.
#define OPUS_DTX 1

// Audio BLE packet configuration
#define AUDIO_PACKET_HEADER_SIZE 3 // 2 bytes index + 1 byte sub-index

// =============================================================================
// VOICE ACTIVITY GATE
// =============================================================================
// Energy gate with hysteresis run on PCM before the encoder. Skipping Opus
// during silence is the point: encoding is the dominant compute cost.
#define VAD_PREROLL_MS 500   // Encode this much audio from before the gate opened
#define VAD_HANGOVER_MS 1200 // Hold the gate open this long after speech stops
#define VAD_OPEN_FACTOR 3.0f // Open when RMS > floor * factor + bias
#define VAD_OPEN_BIAS 180.0f
#define VAD_CLOSE_FACTOR 1.8f // Close when RMS < floor * factor + bias (hysteresis)
#define VAD_CLOSE_BIAS 90.0f
#define VAD_FLOOR_RISE 0.0005f // Noise floor adaptation rate when RMS is above it
#define VAD_FLOOR_FALL 0.05f   // ...and when it is below (track downwards fast)
#define VAD_FLOOR_INIT 300.0f
#define VAD_FLOOR_MIN 40.0f
#define VAD_FLOOR_MAX 6000.0f

#define VAD_PREROLL_FRAMES (VAD_PREROLL_MS / OPUS_FRAME_MS)
#define VAD_HANGOVER_FRAMES (VAD_HANGOVER_MS / OPUS_FRAME_MS)

// =============================================================================
// SD RING BUFFER
// =============================================================================
// Wire-format constants. These are protocol, not preference: the app pins the
// 444-byte record layout in app/test/unit/ring_protocol_test.dart.
#define RING_PAYLOAD_BYTES 440U
#define RING_TIMESTAMP_BYTES 4U
#define RING_RECORD_BYTES (RING_TIMESTAMP_BYTES + RING_PAYLOAD_BYTES)

// Exclusive end offset a frame may reach inside the payload. The app's parser
// (ring_protocol.dart parseAudioPayload) accepts a frame only when
// offset + 1 + size < 440, so a frame's last byte must be index 438 or lower.
// Keeping frame ends at or below 439 satisfies that with no off-by-one.
#define RING_PAYLOAD_FRAME_END_LIMIT 439U

// Bound live-stream latency: a record is only delivered once it is complete, and
// DTX frames are small enough that a record could otherwise hold seconds of
// audio. 20 frames = 400ms worst case.
#define RING_MAX_FRAMES_PER_RECORD 20

// A backlog no larger than this is drained over the live characteristic so the
// delivery cursor can rejoin the writer.
//
// The bound is derived from the app's floor for syncing a ring at all, not
// chosen for comfort: anything the app refuses and we also refuse is drained by
// nobody, and live audio stays off for the rest of the connection.
// refreshWalsFromDevice estimates duration as
// `(440 / (frame_len + 1)) * packets / fps`, which for opusFS320 (frame_len 160,
// fps 50) is 2 frames per record, so its 10-second floor is 250 packets. 256
// clears that with margin.
//
// Note the app's estimate assumes worst-case 160-byte frames; DTX makes real
// frames far smaller, so 256 packed records can be more like a minute of audio
// than ten seconds. Delivering that untimestamped is still much better than the
// alternative, which is never delivering it.
#define LIVE_CATCHUP_MAX_RECORDS 256U

// Records to drain per storage-task pass, so catching up cannot starve the queue
// drain that keeps new audio flowing onto the card.
#define LIVE_CATCHUP_MAX_PER_PASS 4

#define RING_DATA_PATH "/ring.dat"
#define RING_META_PATH "/ring.met"
#define RING_META_SLOT_BYTES 512U
#define RING_META_SLOTS 2U

// Ring sizing. Capacity is fixed once written to metadata so that the
// seq -> offset mapping stays stable across boots.
#define RING_MAX_BYTES (1024ULL * 1024ULL * 1024ULL) // 1 GiB cap (keeps used/free in uint32)
#define RING_CARD_FRACTION_PCT 70                    // ...or this much of the card, whichever is smaller
#define RING_MIN_PACKETS 4096U

// Durability against an abrupt power cut from the slide switch: flush the data
// file and publish new metadata at least this often.
#define RING_SYNC_INTERVAL_MS 1000
#define RING_SYNC_RECORDS 8

// Cadence for persisting the wall clock alone, with no cursor movement to
// justify a write. It only seeds a boot-time estimate, and an idle device would
// otherwise rewrite the same two metadata sectors once a second forever.
#define RING_EPOCH_PERSIST_INTERVAL_MS (5U * 60U * 1000U)

// =============================================================================
// BLE CONFIGURATION
// =============================================================================
#define BLE_MTU_SIZE 517
#define BLE_TX_POWER ESP_PWR_LVL_N0

#define BLE_ADV_MIN_INTERVAL 0x0140 // 200ms
#define BLE_ADV_MAX_INTERVAL 0x0280 // 400ms

// Outstanding un-acked notifications allowed on the bulk sync path. Bulk sync
// must not consume the controller buffers that battery/status notifications need.
#define BLE_BULK_TX_CREDITS 4
#define BLE_BULK_TX_WAIT_MS 200

// =============================================================================
// BLE UUID DEFINITIONS - OMI Protocol
// =============================================================================
#define OMI_SERVICE_UUID "19B10000-E8F2-537E-4F6C-D104768A1214"
#define AUDIO_DATA_UUID "19B10001-E8F2-537E-4F6C-D104768A1214"
#define AUDIO_CODEC_UUID "19B10002-E8F2-537E-4F6C-D104768A1214"

// Deliberately NOT implemented: photo characteristics 19B10005 / 19B10006. The
// app probes 19B10005 and promotes the device to DeviceType.openglass if it
// reads, which routes it to a connector with no storage protocol support.

// Battery Service UUID - Cast to uint16_t for BLE compatibility
#define BATTERY_SERVICE_UUID (uint16_t) 0x180F
#define BATTERY_LEVEL_UUID (uint16_t) 0x2A19

// Time sync service. The app writes a 4-byte little-endian UTC epoch here on
// every connect (OmiDeviceConnection.performSyncTime).
#define TIME_SYNC_SERVICE_UUID "19B10030-E8F2-537E-4F6C-D104768A1214"
#define TIME_SYNC_WRITE_UUID "19B10031-E8F2-537E-4F6C-D104768A1214"
#define TIME_SYNC_READ_UUID "19B10032-E8F2-537E-4F6C-D104768A1214"

// Offline storage (ring buffer) service
#define STORAGE_SERVICE_UUID "30295780-4301-EABD-2904-2849ADFEAE43"
#define STORAGE_CONTROL_UUID "30295781-4301-EABD-2904-2849ADFEAE43"
#define STORAGE_STATUS_UUID "30295782-4301-EABD-2904-2849ADFEAE43"

// OTA Service UUIDs
#define OTA_SERVICE_UUID "19B10010-E8F2-537E-4F6C-D104768A1214"
#define OTA_CONTROL_UUID "19B10011-E8F2-537E-4F6C-D104768A1214" // Write commands, read status
#define OTA_DATA_UUID "19B10012-E8F2-537E-4F6C-D104768A1214"    // Notifications for progress

// OTA Commands (written to OTA_CONTROL_UUID)
#define OTA_CMD_SET_WIFI 0x01   // Set WiFi credentials: [cmd, ssid_len, ssid..., pass_len, pass...]
#define OTA_CMD_START_OTA 0x02  // Start OTA update
#define OTA_CMD_CANCEL_OTA 0x03 // Cancel ongoing OTA
#define OTA_CMD_GET_STATUS 0x04 // Request current status
#define OTA_CMD_SET_URL 0x05    // Set firmware URL: [cmd, url_len_be16, url...]

// OTA Status codes (notified via OTA_DATA_UUID)
#define OTA_STATUS_IDLE 0x00
#define OTA_STATUS_WIFI_CONNECTING 0x10
#define OTA_STATUS_WIFI_CONNECTED 0x11
#define OTA_STATUS_WIFI_FAILED 0x12
#define OTA_STATUS_DOWNLOADING 0x20 // Followed by progress byte (0-100)
#define OTA_STATUS_DOWNLOAD_COMPLETE 0x21
#define OTA_STATUS_DOWNLOAD_FAILED 0x22
#define OTA_STATUS_INSTALLING 0x30 // Followed by progress byte (0-100)
#define OTA_STATUS_INSTALL_COMPLETE 0x31
#define OTA_STATUS_INSTALL_FAILED 0x32
#define OTA_STATUS_REBOOTING 0x40
#define OTA_STATUS_ERROR 0xFF

// WiFi Configuration. WiFi carries OTA only: it is brought up inside the OTA
// task and torn back down when the task ends. There is no WiFi audio path.
#define WIFI_CONNECT_TIMEOUT_MS 15000
#define WIFI_MAX_SSID_LEN 32
#define WIFI_MAX_PASS_LEN 64
#define OTA_MAX_URL_LEN 256

// =============================================================================
// TASK CONFIGURATION
// =============================================================================
#define AUDIO_TASK_STACK_SIZE 6144
#define AUDIO_TASK_PRIORITY 6
#define STORAGE_TASK_STACK_SIZE 8192
#define STORAGE_TASK_PRIORITY 4
#define RECORD_QUEUE_DEPTH 10

#endif // CONFIG_H

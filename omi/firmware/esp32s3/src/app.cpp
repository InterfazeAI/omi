#include "app.h"

#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

#include "config.h"
#include "mic.h"
#include "opus_encoder.h"
#include "ota.h"
#include "rtc_clock.h"
#include "sd_ring.h"
#include "storage_service.h"
#include "vad.h"

// Device Information Service UUIDs
#define DEVICE_INFORMATION_SERVICE_UUID (uint16_t) 0x180A
#define MANUFACTURER_NAME_STRING_CHAR_UUID (uint16_t) 0x2A29
#define MODEL_NUMBER_STRING_CHAR_UUID (uint16_t) 0x2A24
#define FIRMWARE_REVISION_STRING_CHAR_UUID (uint16_t) 0x2A26
#define HARDWARE_REVISION_STRING_CHAR_UUID (uint16_t) 0x2A27
#define SERIAL_NUMBER_STRING_CHAR_UUID (uint16_t) 0x2A25

// Battery state
static float batteryVoltage = 0.0f;
static int batteryPercentage = 0;
static unsigned long lastBatteryCheck = 0;

// Connection state
static volatile bool connected = false;
static volatile bool audioSubscribed = false;
static volatile uint16_t audioPacketIndex = 0;

// Power state
static unsigned long lastActivity = 0;
static bool powerSaveMode = false;

// Set by the time sync BLE write; acted on by the storage task so no SD I/O ever
// happens on the Bluetooth stack's task.
static volatile bool timeSyncPending = false;

// LED
typedef enum { LED_BOOT_SEQUENCE, LED_NORMAL_OPERATION } led_mode_t;
static led_mode_t ledMode = LED_BOOT_SEQUENCE;

// Characteristics
static BLEServer *bleServer = nullptr;
static BLECharacteristic *batteryLevelCharacteristic = nullptr;
static BLECharacteristic *audioDataCharacteristic = nullptr;
static BLECharacteristic *audioCodecCharacteristic = nullptr;
static BLECharacteristic *timeSyncReadCharacteristic = nullptr;
static BLECharacteristic *otaControlCharacteristic = nullptr;
static BLECharacteristic *otaDataCharacteristic = nullptr;

// Completed records handed from the audio task to the storage task.
//
// Whether the timestamp came from a synced clock travels *with* the record
// rather than being decided when the storage task drains it. The audio task is
// the only place that knows — it stamps the record when its first frame lands —
// and the two tasks are not ordered against each other, so asking later would
// vouch for a record that was stamped from the pre-sync estimate.
typedef struct {
    uint8_t bytes[RING_RECORD_BYTES];
    bool timestamp_authoritative;
} queued_record_t;

static QueueHandle_t recordQueue = nullptr;
static volatile uint32_t recordsDroppedByQueue = 0;

// Record under construction, owned exclusively by the audio task.
static queued_record_t pendingRecord;
static uint16_t recordOffset = 0;
static uint16_t recordFrames = 0;
static uint32_t recordTimestamp = 0;

static uint8_t audioPacketBuffer[OPUS_OUTPUT_MAX_BYTES + AUDIO_PACKET_HEADER_SIZE];

static void readBatteryLevel();
static void updateBatteryService();

// -------------------------------------------------------------------------
// LED
// -------------------------------------------------------------------------
static void ledWrite(bool on)
{
#if STATUS_LED_ACTIVE_LOW
    digitalWrite(STATUS_LED_PIN, on ? LOW : HIGH);
#else
    digitalWrite(STATUS_LED_PIN, on ? HIGH : LOW);
#endif
}

static void updateLED()
{
    const unsigned long now = millis();
    static unsigned long bootStartTime = 0;

    if (ledMode == LED_BOOT_SEQUENCE) {
        if (bootStartTime == 0) {
            bootStartTime = now;
        }
        if (now - bootStartTime < 1500) {
            ledWrite(((now - bootStartTime) / 150) % 2 == 0);
            return;
        }
        ledMode = LED_NORMAL_OPERATION;
    }

    if (!sd_ring_is_ready()) {
        // No usable card: everything else still works, but offline recording is
        // gone, which is worth signalling rather than failing silently.
        ledWrite((now / 150) % 2 == 0);
    } else if (connected) {
        ledWrite(true);
    } else {
        ledWrite((now / 1000) % 2 == 0);
    }
}

// -------------------------------------------------------------------------
// Power management
//
// There is no button and no wake source, so "off" is the slide switch and deep
// sleep would be a one-way trip. Light sleep is out too: it gates the I2S clock,
// and the voice gate needs the mic sampling continuously to catch a speech onset
// at all. What is left is CPU frequency scaling, driven purely by idle time and
// connection state.
// -------------------------------------------------------------------------
static void enterPowerSave()
{
    if (!powerSaveMode) {
        setCpuFrequencyMhz(MIN_CPU_FREQ_MHZ);
        powerSaveMode = true;
    }
}

static void exitPowerSave()
{
    if (powerSaveMode) {
        setCpuFrequencyMhz(NORMAL_CPU_FREQ_MHZ);
        powerSaveMode = false;
    }
}

static void updatePowerState()
{
    const unsigned long now = millis();
    const bool busy = connected || vad_is_open() || ota_is_busy();

    if (busy) {
        lastActivity = now;
        exitPowerSave();
    } else if (now - lastActivity > IDLE_THRESHOLD_MS) {
        enterPowerSave();
    }
}

// -------------------------------------------------------------------------
// Live audio path
// -------------------------------------------------------------------------
static bool broadcastAudioFrame(const uint8_t *data, size_t len)
{
    if (!connected || !audioSubscribed || audioDataCharacteristic == nullptr) {
        return false;
    }
    // The encoder cannot produce a frame this large, but a length byte read back
    // off the card can be anything: a record torn by a power cut, or stale bytes
    // left behind by a filesystem rebuild. Drop it rather than memcpy past the
    // packet buffer. Skipping the frame is enough — the caller walks the rest of
    // the payload, and a malformed record costs at most its own audio.
    if (len == 0 || len > OPUS_OUTPUT_MAX_BYTES) {
        return true;
    }

    audioPacketBuffer[0] = (uint8_t) (audioPacketIndex & 0xFF);
    audioPacketBuffer[1] = (uint8_t) ((audioPacketIndex >> 8) & 0xFF);
    audioPacketBuffer[2] = 0; // sub-index; frames always fit one notification
    memcpy(audioPacketBuffer + AUDIO_PACKET_HEADER_SIZE, data, len);

    audioDataCharacteristic->setValue(audioPacketBuffer, len + AUDIO_PACKET_HEADER_SIZE);
    audioDataCharacteristic->notify();
    audioPacketIndex++;
    return true;
}

// Unpack a journalled record and stream its frames live. The walk mirrors the
// app's parseAudioPayload exactly, including the `>=` boundary, so what plays
// live is byte-for-byte what a later sync of the same record would deliver.
static bool liveSendRecord(const uint8_t *record)
{
    const uint8_t *payload = record + RING_TIMESTAMP_BYTES;
    uint16_t offset = 0;

    while (offset < RING_PAYLOAD_BYTES - 1U) {
        const uint8_t len = payload[offset];
        if (len == 0) {
            offset++;
            continue;
        }
        if ((uint16_t) (offset + 1U + len) >= RING_PAYLOAD_BYTES) {
            break;
        }
        if (!broadcastAudioFrame(payload + offset + 1U, len)) {
            return false;
        }
        offset = (uint16_t) (offset + 1U + len);
    }
    return true;
}

// -------------------------------------------------------------------------
// Record assembly (audio task)
// -------------------------------------------------------------------------
static void recordReset()
{
    memset(&pendingRecord, 0, sizeof(pendingRecord));
    recordOffset = 0;
    recordFrames = 0;
    recordTimestamp = 0;
}

static void recordFlush()
{
    if (recordFrames == 0) {
        return;
    }

    pendingRecord.bytes[0] = (uint8_t) (recordTimestamp >> 24);
    pendingRecord.bytes[1] = (uint8_t) (recordTimestamp >> 16);
    pendingRecord.bytes[2] = (uint8_t) (recordTimestamp >> 8);
    pendingRecord.bytes[3] = (uint8_t) recordTimestamp;

    if (recordQueue == nullptr || xQueueSend(recordQueue, &pendingRecord, 0) != pdTRUE) {
        recordsDroppedByQueue++;
    }
    recordReset();
}

static void onGatedFrame(const int16_t *pcm, size_t samples, uint32_t age_ms, bool session_start)
{
    (void) samples;

    // A session is one contiguous open period of the gate, and its first record
    // carries the wall-clock time speech actually started. Closing the previous
    // record here is what keeps that timestamp meaningful instead of letting two
    // sessions share a record.
    if (session_start) {
        recordFlush();
    }

    const int len = opus_encode_frame(pcm);
    // 0xFF is a reserved escape in the sibling storage protocol, so never emit it
    // as a length byte even though this bitrate cannot produce a frame that big.
    if (len <= 0 || len >= 0xFF) {
        return;
    }

    if ((uint16_t) (recordOffset + 1U + len) > RING_PAYLOAD_FRAME_END_LIMIT) {
        recordFlush();
    }

    if (recordFrames == 0) {
        const uint32_t now = rtc_now();
        const uint32_t age_s = (age_ms + 500U) / 1000U;
        recordTimestamp = (now > age_s) ? (now - age_s) : now;
        pendingRecord.timestamp_authoritative = rtc_is_valid();
    }

    uint8_t *payload = pendingRecord.bytes + RING_TIMESTAMP_BYTES;
    payload[recordOffset] = (uint8_t) len;
    memcpy(payload + recordOffset + 1U, opus_frame_data(), (size_t) len);
    recordOffset = (uint16_t) (recordOffset + 1U + len);
    recordFrames++;

    if (recordFrames >= RING_MAX_FRAMES_PER_RECORD) {
        recordFlush();
    }
}

static void onMicData(int16_t *data, size_t samples)
{
    opus_receive_pcm(data, samples);
}

static void audioTask(void *param)
{
    (void) param;
    static int16_t frame[OPUS_FRAME_SAMPLES];
    uint32_t lastClockGeneration = rtc_generation();

    for (;;) {
        // A time sync moves wall time discontinuously, potentially by hours. Close
        // the record in progress so none carries a timestamp taken before the jump
        // while sitting above the authoritative watermark.
        const uint32_t clockGeneration = rtc_generation();
        if (clockGeneration != lastClockGeneration) {
            lastClockGeneration = clockGeneration;
            recordFlush();
        }

        mic_process();
        while (opus_pcm_pop_frame(frame)) {
            vad_process_frame(frame);
        }
        // Gate shut: hand the tail of the session over rather than holding a
        // partial record until whenever someone next speaks.
        if (!vad_is_open()) {
            recordFlush();
        }
        vTaskDelay(1);
    }
}

// -------------------------------------------------------------------------
// Journalling and delivery (storage task)
//
// Every frame is journalled first, and `read_seq` alone decides which BLE path
// delivers it. That is what makes double delivery structurally impossible: the
// app runs live capture and offline sync concurrently with no arbitration of its
// own, and its only dedup is an exact WAL-id collision.
// -------------------------------------------------------------------------
static bool liveDrainAllowed()
{
    return connected && audioSubscribed && !storage_service_sync_busy();
}

static void journalRecord(const queued_record_t *item)
{
    if (!sd_ring_is_ready()) {
        return;
    }

    sd_ring_info_t before;
    sd_ring_get_info(&before);
    const bool caughtUp = (before.read_seq == before.write_seq);

    // Plant the watermark immediately before the write, so it lands exactly on
    // the first record whose timestamp came from a synced clock. The call is
    // idempotent; only the first authoritative record moves it.
    if (item->timestamp_authoritative) {
        sd_ring_mark_timestamps_authoritative();
    }

    const uint64_t seq = sd_ring_write_record(item->bytes);
    if (seq == SD_RING_SEQ_INVALID) {
        return;
    }

    // Fast path: no backlog, so the record just written is the one to send and it
    // is already in RAM. With a backlog we leave it unread — the app pulls it
    // through the ring protocol, which carries the per-record timestamps that
    // live streaming cannot. liveCatchUp handles closing a small backlog.
    if (!caughtUp || !liveDrainAllowed()) {
        return;
    }
    if (liveSendRecord(item->bytes)) {
        sd_ring_advance_lazy(seq + 1U);
    }
}

// Stream the oldest unread records so the cursor can catch up to the writer.
//
// Without this, live audio stops for good after the first offline sync: the app
// advances only to the sequence the transfer covered, while the VAD keeps
// journalling throughout, so a residual backlog is guaranteed and `caughtUp`
// never becomes true again. The app will not clear it either — it skips rings
// holding under ten seconds of audio.
//
// Records drained this way lose their timestamps, since the live characteristic
// has nowhere to put one. That is why the backlog has to be small: it is filed
// as arriving now, which is only honest for audio that is nearly current.
static void liveCatchUp()
{
    if (!sd_ring_is_ready() || !liveDrainAllowed()) {
        return;
    }

    static uint8_t staged[RING_RECORD_BYTES];

    for (int sent = 0; sent < LIVE_CATCHUP_MAX_PER_PASS; sent++) {
        sd_ring_info_t info;
        sd_ring_get_info(&info);
        const uint64_t backlog = info.write_seq - info.read_seq;
        if (backlog == 0U || backlog > LIVE_CATCHUP_MAX_RECORDS) {
            return;
        }

        uint32_t bytes_read = 0;
        uint32_t packets_read = 0;
        if (sd_ring_read(info.read_seq, staged, RING_RECORD_BYTES, &bytes_read, &packets_read) < 0 ||
            packets_read == 0U) {
            return;
        }
        if (!liveSendRecord(staged)) {
            return;
        }
        sd_ring_advance_lazy(info.read_seq + 1U);
    }
}

static void storageTask(void *param)
{
    (void) param;
    static queued_record_t record;

    sd_ring_init();
    rtc_restore(sd_ring_restored_epoch());

    for (;;) {
        int drained = 0;
        while (drained < 8 && xQueueReceive(recordQueue, &record, 0) == pdTRUE) {
            journalRecord(&record);
            drained++;
        }

        // Persist the freshly synced epoch straight away; the watermark itself is
        // planted per record in journalRecord.
        if (timeSyncPending) {
            timeSyncPending = false;
            sd_ring_tick(rtc_now(), true);
        }

        liveCatchUp();
        storage_service_process();
        sd_ring_tick(rtc_now(), false);

        if (drained == 0 && !storage_service_transfer_active()) {
            vTaskDelay(pdMS_TO_TICKS(connected ? 5 : 20));
        } else {
            taskYIELD();
        }
    }
}

// -------------------------------------------------------------------------
// BLE callbacks
// -------------------------------------------------------------------------
class ServerHandler : public BLEServerCallbacks
{
    void onConnect(BLEServer *server, esp_ble_gatts_cb_param_t *param) override
    {
        connected = true;
        audioSubscribed = false;
        lastActivity = millis();
        storage_service_on_connect(param->connect.conn_id);
        Serial.println(">>> BLE client connected");
        updateBatteryService();
    }

    void onDisconnect(BLEServer *server) override
    {
        (void) server;
        connected = false;
        audioSubscribed = false;
        storage_service_on_disconnect();
        Serial.println("<<< BLE client disconnected, restarting advertising");
        BLEDevice::startAdvertising();
    }
};

class AudioCCCDCallback : public BLEDescriptorCallbacks
{
    void onWrite(BLEDescriptor *descriptor) override
    {
        uint8_t *value = descriptor->getValue();
        if (value != nullptr && descriptor->getLength() >= 2) {
            audioSubscribed = (value[0] & 0x01) != 0;
            Serial.printf("Audio notifications %s\n", audioSubscribed ? "enabled" : "disabled");
        }
    }
};

class TimeSyncWriteCallback : public BLECharacteristicCallbacks
{
    void onWrite(BLECharacteristic *characteristic) override
    {
        const std::string value = characteristic->getValue();
        if (value.length() < 4) {
            return;
        }
        const uint8_t *b = (const uint8_t *) value.data();
        // Little-endian: OmiDeviceConnection.performSyncTime writes
        // ByteData(4)..setUint32(0, epochSeconds, Endian.little).
        const uint32_t epoch =
            (uint32_t) b[0] | ((uint32_t) b[1] << 8) | ((uint32_t) b[2] << 16) | ((uint32_t) b[3] << 24);
        if (rtc_set_utc(epoch)) {
            timeSyncPending = true;
        }
    }
};

// -------------------------------------------------------------------------
// Battery
// -------------------------------------------------------------------------
static void readBatteryLevel()
{
    int adcSum = 0;
    for (int i = 0; i < 10; i++) {
        adcSum += analogRead(BATTERY_ADC_PIN);
    }
    const int adcValue = adcSum / 10;

    const float adcVoltage = (adcValue / 4095.0f) * 3.3f;
    batteryVoltage = adcVoltage * VOLTAGE_DIVIDER_RATIO;

    if (batteryVoltage > 5.0f) {
        batteryVoltage = 5.0f;
    }
    if (batteryVoltage < 2.5f) {
        batteryVoltage = 2.5f;
    }

    if (batteryVoltage >= BATTERY_MAX_VOLTAGE) {
        batteryPercentage = 100;
    } else if (batteryVoltage <= BATTERY_MIN_VOLTAGE) {
        batteryPercentage = 0;
    } else {
        const float range = BATTERY_MAX_VOLTAGE - BATTERY_MIN_VOLTAGE;
        batteryPercentage = (int) (((batteryVoltage - BATTERY_MIN_VOLTAGE) / range) * 100.0f);
    }

    // Slew-limit so a momentary sag under load does not jump the reported level.
    static int lastBatteryPercentage = -1;
    if (lastBatteryPercentage >= 0 && abs(batteryPercentage - lastBatteryPercentage) > 5) {
        batteryPercentage = lastBatteryPercentage + (batteryPercentage > lastBatteryPercentage ? 2 : -2);
    }
    if (batteryPercentage > 100) {
        batteryPercentage = 100;
    }
    if (batteryPercentage < 0) {
        batteryPercentage = 0;
    }
    lastBatteryPercentage = batteryPercentage;
}

static void updateBatteryService()
{
    if (batteryLevelCharacteristic == nullptr) {
        return;
    }
    const uint8_t level = (uint8_t) batteryPercentage;
    batteryLevelCharacteristic->setValue((uint8_t *) &level, 1);
    if (connected) {
        batteryLevelCharacteristic->notify();
    }
}

// -------------------------------------------------------------------------
// BLE setup
// -------------------------------------------------------------------------
static void configure_ble()
{
    Serial.println("Initializing BLE...");
    BLEDevice::init(BLE_DEVICE_NAME);
    // BLE_MTU_SIZE existed in the omiGlass config but was never applied, leaving
    // every transfer on the 23-byte default.
    BLEDevice::setMTU(BLE_MTU_SIZE);
    BLEDevice::setPower(BLE_TX_POWER);

    bleServer = BLEDevice::createServer();
    bleServer->setCallbacks(new ServerHandler());

    // Main Omi service. Deliberately no photo characteristics: the app promotes
    // any device that answers 19B10005 to DeviceType.openglass, whose connector
    // does not implement the storage protocol.
    BLEService *service = bleServer->createService(BLEUUID(OMI_SERVICE_UUID));

    audioDataCharacteristic = service->createCharacteristic(
        BLEUUID(AUDIO_DATA_UUID), BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
    BLE2902 *audioCcc = new BLE2902();
    audioCcc->setNotifications(true);
    audioCcc->setCallbacks(new AudioCCCDCallback());
    audioDataCharacteristic->addDescriptor(audioCcc);

    audioCodecCharacteristic =
        service->createCharacteristic(BLEUUID(AUDIO_CODEC_UUID), BLECharacteristic::PROPERTY_READ);
    uint8_t codecId = opus_get_codec_id();
    audioCodecCharacteristic->setValue(&codecId, 1);

    // Battery Service
    BLEService *batteryService = bleServer->createService(BATTERY_SERVICE_UUID);
    batteryLevelCharacteristic = batteryService->createCharacteristic(
        BATTERY_LEVEL_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
    BLE2902 *batteryCcc = new BLE2902();
    batteryCcc->setNotifications(true);
    batteryLevelCharacteristic->addDescriptor(batteryCcc);
    readBatteryLevel();
    uint8_t initialBatteryLevel = (uint8_t) batteryPercentage;
    batteryLevelCharacteristic->setValue(&initialBatteryLevel, 1);

    // Device Information Service
    BLEService *deviceInfoService = bleServer->createService(DEVICE_INFORMATION_SERVICE_UUID);
    deviceInfoService->createCharacteristic(MANUFACTURER_NAME_STRING_CHAR_UUID, BLECharacteristic::PROPERTY_READ)
        ->setValue(MANUFACTURER_NAME);
    deviceInfoService->createCharacteristic(MODEL_NUMBER_STRING_CHAR_UUID, BLECharacteristic::PROPERTY_READ)
        ->setValue(BLE_DEVICE_NAME);
    deviceInfoService->createCharacteristic(FIRMWARE_REVISION_STRING_CHAR_UUID, BLECharacteristic::PROPERTY_READ)
        ->setValue(FIRMWARE_VERSION_STRING);
    deviceInfoService->createCharacteristic(HARDWARE_REVISION_STRING_CHAR_UUID, BLECharacteristic::PROPERTY_READ)
        ->setValue(HARDWARE_REVISION);

    const uint64_t chipId = ESP.getEfuseMac();
    char serialNumber[17];
    snprintf(serialNumber, sizeof(serialNumber), "%04X%08X", (uint16_t) (chipId >> 32), (uint32_t) chipId);
    deviceInfoService->createCharacteristic(SERIAL_NUMBER_STRING_CHAR_UUID, BLECharacteristic::PROPERTY_READ)
        ->setValue(serialNumber);

    // Time sync service. Without it every record timestamp is a guess.
    BLEService *timeSyncService = bleServer->createService(BLEUUID(TIME_SYNC_SERVICE_UUID));
    BLECharacteristic *timeSyncWrite = timeSyncService->createCharacteristic(
        BLEUUID(TIME_SYNC_WRITE_UUID), BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    timeSyncWrite->setCallbacks(new TimeSyncWriteCallback());
    timeSyncReadCharacteristic =
        timeSyncService->createCharacteristic(BLEUUID(TIME_SYNC_READ_UUID), BLECharacteristic::PROPERTY_READ);
    uint8_t epochBytes[4] = {0, 0, 0, 0};
    timeSyncReadCharacteristic->setValue(epochBytes, sizeof(epochBytes));

    // OTA Service
    BLEService *otaService = bleServer->createService(BLEUUID(OTA_SERVICE_UUID));
    otaControlCharacteristic = otaService->createCharacteristic(
        BLEUUID(OTA_CONTROL_UUID), BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
    otaControlCharacteristic->setCallbacks(ota_create_control_callback());
    otaDataCharacteristic = otaService->createCharacteristic(
        BLEUUID(OTA_DATA_UUID), BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
    BLE2902 *otaCcc = new BLE2902();
    otaCcc->setNotifications(true);
    otaDataCharacteristic->addDescriptor(otaCcc);
    ota_set_characteristics(otaControlCharacteristic, otaDataCharacteristic);

    // Offline storage (starts its own service internally)
    storage_service_init(bleServer);

    service->start();
    batteryService->start();
    deviceInfoService->start();
    timeSyncService->start();
    otaService->start();

    // The Omi service UUID must ride in the advertising packet itself, not the
    // scan response: native_bluetooth_discoverer.dart filters on it and nothing
    // else. Moving the name to the scan response keeps the 31-byte budget.
    BLEAdvertising *advertising = BLEDevice::getAdvertising();
    advertising->addServiceUUID(BLEUUID(OMI_SERVICE_UUID));
    advertising->setScanResponse(true);
    advertising->setMinPreferred(BLE_ADV_MIN_INTERVAL);
    advertising->setMaxPreferred(BLE_ADV_MAX_INTERVAL);
    BLEDevice::startAdvertising();

    Serial.println("BLE initialized and advertising");
}

static void updateTimeSyncReadback()
{
    if (timeSyncReadCharacteristic == nullptr) {
        return;
    }
    const uint32_t epoch = rtc_now();
    uint8_t bytes[4] = {(uint8_t) epoch, (uint8_t) (epoch >> 8), (uint8_t) (epoch >> 16), (uint8_t) (epoch >> 24)};
    timeSyncReadCharacteristic->setValue(bytes, sizeof(bytes));
}

// -------------------------------------------------------------------------
// Setup & loop
// -------------------------------------------------------------------------
void setup_app()
{
    Serial.begin(921600);
    delay(100);
    Serial.println("\nOmi ESP32-S3 pendant starting...");

    pinMode(STATUS_LED_PIN, OUTPUT);
    ledWrite(false);
    ledMode = LED_BOOT_SEQUENCE;

    setCpuFrequencyMhz(NORMAL_CPU_FREQ_MHZ);
    lastActivity = millis();

    analogReadResolution(12);
    analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);
    readBatteryLevel();

    recordQueue = xQueueCreate(RECORD_QUEUE_DEPTH, sizeof(queued_record_t));
    if (recordQueue == nullptr) {
        Serial.println("Failed to create record queue!");
    }
    recordReset();

    configure_ble();

    if (!opus_encoder_init()) {
        Serial.println("Failed to initialize Opus encoder!");
    }
    if (!vad_init()) {
        Serial.println("Failed to initialize voice gate!");
    }
    vad_set_callback(onGatedFrame);

    if (mic_start()) {
        mic_set_callback(onMicData);
    } else {
        Serial.println("Failed to start microphone!");
    }

    // The card is mounted inside the storage task rather than here: flipping the
    // switch is a cold boot with nobody watching, so BLE has to come up and start
    // advertising even if the card is slow, missing, or needs reformatting.
    xTaskCreatePinnedToCore(
        storageTask, "storage", STORAGE_TASK_STACK_SIZE, nullptr, STORAGE_TASK_PRIORITY, nullptr, 1);
    xTaskCreatePinnedToCore(audioTask, "audio", AUDIO_TASK_STACK_SIZE, nullptr, AUDIO_TASK_PRIORITY, nullptr, 1);

    Serial.println("Setup complete.");
}

void loop_app()
{
    const unsigned long now = millis();

    updateLED();
    ota_loop();
    updatePowerState();

    if (now - lastBatteryCheck >= BATTERY_TASK_INTERVAL_MS) {
        lastBatteryCheck = now;
        readBatteryLevel();
        updateBatteryService();
    }

    static unsigned long lastReadback = 0;
    if (now - lastReadback >= 1000) {
        lastReadback = now;
        updateTimeSyncReadback();
    }

    static unsigned long lastStats = 0;
    if (now - lastStats >= 30000) {
        lastStats = now;
        sd_ring_info_t info;
        sd_ring_get_info(&info);
        Serial.printf("[stat] batt %d%% (%.2fV) gate=%d rms=%.0f floor=%.0f read=%llu write=%llu dropped=%llu q=%u\n",
                      batteryPercentage,
                      batteryVoltage,
                      vad_is_open() ? 1 : 0,
                      vad_last_rms(),
                      vad_noise_floor(),
                      (unsigned long long) info.read_seq,
                      (unsigned long long) info.write_seq,
                      (unsigned long long) info.dropped_packets,
                      (unsigned) recordsDroppedByQueue);
    }

    delay(20);
}

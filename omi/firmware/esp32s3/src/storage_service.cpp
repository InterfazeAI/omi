#include "storage_service.h"

#include <BLE2902.h>
#include <BLECharacteristic.h>
#include <BLEDevice.h>

#include "config.h"
#include "sd_ring.h"

#define CMD_STOP_SYNC 0x03
#define CMD_RING_INFO 0x10
#define CMD_RING_READ 0x11
#define CMD_RING_ADVANCE 0x12
#define CMD_RING_CLEAR 0x13

#define STATUS_OK 0
#define STATUS_INVALID_COMMAND 6
#define STATUS_NOT_READY 9
#define STATUS_SEQ_OUT_OF_RANGE 10

#define NOTIFY_ACK 0x01
#define NOTIFY_INFO 0x02
#define NOTIFY_DATA 0x03
#define NOTIFY_DONE 0x04
#define NOTIFY_READ_BEGIN 0x05

// Packets pulled from the card per process() call. Small on purpose: the
// storage task interleaves draining the audio record queue between batches, so
// a long sync cannot starve recording.
#define STORAGE_BATCH_PACKETS 8U
#define STORAGE_STATUS_REFRESH_MS 250U

// The card may still be remounting when the app asks to sync. The app only
// triggers sync once per connect, so hold the request briefly instead of
// answering "not ready" and having it give up.
#define STORAGE_SD_READY_TIMEOUT_MS 5000U

// How long a latched sync intent holds the live drain off with no follow-up
// command. Generous next to the app's own 5s no-data timeout, but bounded so a
// peer that asks for INFO and then goes quiet cannot disable live audio.
#define STORAGE_SYNC_INTENT_TIMEOUT_MS 15000U

static BLEServer *ble_server = nullptr;
static BLECharacteristic *control_char = nullptr;
static BLECharacteristic *status_char = nullptr;

static volatile uint16_t peer_conn_id = 0;
static volatile bool peer_connected = false;

static volatile bool info_requested = false;
static volatile bool clear_requested = false;
static volatile bool read_requested = false;
static volatile bool advance_requested = false;
static volatile bool stop_requested = false;

static volatile uint64_t pending_start_seq = 0;
static volatile uint32_t pending_packet_count = 0;
static volatile uint64_t pending_advance_seq = 0;

// Acks the write callback wants sent. The callback runs on the Bluetooth stack's
// task and must not touch the control characteristic itself: the storage task is
// concurrently driving the same characteristic through push_transfer_batch, and
// an interleaved setValue would substitute an ack for a DATA chunk. DATA is not
// record-aligned, so losing one chunk misaligns the app's reassembler for the
// rest of the transfer instead of just dropping a record.
#define ACK_QUEUE_DEPTH 4
static volatile uint8_t ack_queue[ACK_QUEUE_DEPTH];
static volatile uint8_t ack_queue_head = 0;
static volatile uint8_t ack_queue_tail = 0;

static bool transfer_active = false;
static bool read_begin_sent = false;
static bool done_pending = false;
static uint64_t transfer_start_seq = 0;
static uint64_t current_read_seq = 0;
static uint32_t remaining_packets = 0;
static uint8_t transfer_end_status = STATUS_OK;

static uint32_t info_deadline_ms = 0;
static uint32_t read_deadline_ms = 0;
static uint32_t status_refresh_deadline_ms = 0;

/// millis() after which a latched sync intent lapses, or 0 for none.
static volatile uint32_t sync_intent_deadline_ms = 0;

static void latch_sync_intent()
{
    uint32_t deadline = millis() + STORAGE_SYNC_INTENT_TIMEOUT_MS;
    if (deadline == 0U) {
        deadline = 1U; // 0 is the "no intent" sentinel.
    }
    sync_intent_deadline_ms = deadline;
}

static void clear_sync_intent()
{
    sync_intent_deadline_ms = 0;
}

static uint8_t batch_buffer[STORAGE_BATCH_PACKETS * RING_RECORD_BYTES];
static uint8_t control_notify_buf[32];
static uint8_t data_notify_buf[BLE_MTU_SIZE];

// ---------------------------------------------------------------------------
// Byte order helpers (the protocol is big-endian everywhere except the status
// read, which is little-endian because that is what the reference firmware
// emits and the app parses).
// ---------------------------------------------------------------------------

static void put_be16(uint8_t *dst, uint16_t v)
{
    dst[0] = (uint8_t) (v >> 8);
    dst[1] = (uint8_t) v;
}

static void put_be32(uint8_t *dst, uint32_t v)
{
    dst[0] = (uint8_t) (v >> 24);
    dst[1] = (uint8_t) (v >> 16);
    dst[2] = (uint8_t) (v >> 8);
    dst[3] = (uint8_t) v;
}

static void put_be64(uint8_t *dst, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        dst[i] = (uint8_t) (v >> (56 - 8 * i));
    }
}

static uint32_t get_be32(const uint8_t *src)
{
    return ((uint32_t) src[0] << 24) | ((uint32_t) src[1] << 16) | ((uint32_t) src[2] << 8) | (uint32_t) src[3];
}

static uint64_t get_be64(const uint8_t *src)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v = (v << 8) | (uint64_t) src[i];
    }
    return v;
}

static void put_le32(uint8_t *dst, uint32_t v)
{
    dst[0] = (uint8_t) v;
    dst[1] = (uint8_t) (v >> 8);
    dst[2] = (uint8_t) (v >> 16);
    dst[3] = (uint8_t) (v >> 24);
}

static uint8_t status_from_ring_error(int err)
{
    switch (err) {
    case SD_RING_ERR_RANGE:
        return STATUS_SEQ_OUT_OF_RANGE;
    default:
        return STATUS_NOT_READY;
    }
}

// ---------------------------------------------------------------------------
// Notification plumbing
// ---------------------------------------------------------------------------

static uint16_t data_chunk_size()
{
    uint16_t mtu = 23;
    if (ble_server != nullptr && peer_connected) {
        const uint16_t negotiated = ble_server->getPeerMTU(peer_conn_id);
        if (negotiated > mtu) {
            mtu = negotiated;
        }
    }
    if (mtu > BLE_MTU_SIZE) {
        mtu = BLE_MTU_SIZE;
    }
    // 3 bytes of ATT notification header, 1 byte of protocol opcode.
    return (uint16_t) (mtu - 4U);
}

static bool notify_control(const uint8_t *data, size_t len)
{
    if (control_char == nullptr || !peer_connected) {
        return false;
    }
    control_char->setValue((uint8_t *) data, len);
    control_char->notify();
    return true;
}

static bool send_ack(uint8_t status)
{
    control_notify_buf[0] = NOTIFY_ACK;
    control_notify_buf[1] = status;
    return notify_control(control_notify_buf, 2);
}

/// Queue an ack from the Bluetooth stack's task for the storage task to send.
static void queue_ack(uint8_t status)
{
    const uint8_t next = (uint8_t) ((ack_queue_tail + 1U) % ACK_QUEUE_DEPTH);
    if (next == ack_queue_head) {
        return; // Peer is spamming commands; dropping an ack beats blocking here.
    }
    ack_queue[ack_queue_tail] = status;
    ack_queue_tail = next;
}

static void flush_queued_acks()
{
    while (ack_queue_head != ack_queue_tail) {
        const uint8_t status = ack_queue[ack_queue_head];
        ack_queue_head = (uint8_t) ((ack_queue_head + 1U) % ACK_QUEUE_DEPTH);
        send_ack(status);
    }
}

static bool send_done(uint8_t status, uint64_t next_seq)
{
    control_notify_buf[0] = NOTIFY_DONE;
    control_notify_buf[1] = status;
    put_be64(control_notify_buf + 2, next_seq);
    return notify_control(control_notify_buf, 10);
}

static bool send_info()
{
    if (!sd_ring_is_ready()) {
        return send_ack(STATUS_NOT_READY);
    }
    sd_ring_info_t info;
    sd_ring_get_info(&info);

    control_notify_buf[0] = NOTIFY_INFO;
    put_be64(control_notify_buf + 1, info.read_seq);
    put_be64(control_notify_buf + 9, info.write_seq);
    put_be32(control_notify_buf + 17, info.capacity_packets);
    put_be64(control_notify_buf + 21, info.dropped_packets);
    put_be16(control_notify_buf + 29, (uint16_t) RING_RECORD_BYTES);
    return notify_control(control_notify_buf, 31);
}

// ---------------------------------------------------------------------------
// Transfer state
// ---------------------------------------------------------------------------

static void reset_transfer()
{
    transfer_active = false;
    read_begin_sent = false;
    done_pending = false;
    transfer_start_seq = 0;
    current_read_seq = 0;
    remaining_packets = 0;
    transfer_end_status = STATUS_OK;
}

static bool consume_stop()
{
    if (!stop_requested) {
        return false;
    }
    stop_requested = false;
    clear_sync_intent();
    reset_transfer();
    return true;
}

static void start_pending_read()
{
    sd_ring_info_t info;
    sd_ring_get_info(&info);

    const uint64_t start = pending_start_seq;
    if (start < info.read_seq || start > info.write_seq) {
        send_ack(STATUS_SEQ_OUT_OF_RANGE);
        return;
    }

    uint64_t available = info.write_seq - start;
    uint32_t requested = pending_packet_count;
    if (requested == 0U || (uint64_t) requested > available) {
        requested = (available > UINT32_MAX) ? UINT32_MAX : (uint32_t) available;
    }

    transfer_active = true;
    read_begin_sent = false;
    done_pending = false;
    transfer_start_seq = start;
    current_read_seq = start;
    remaining_packets = requested;
    transfer_end_status = STATUS_OK;
}

// Push one batch of the running transfer. DATA notifications are deliberately
// not aligned to record boundaries — the app's RingRecordReassembler puts them
// back together, which lets us fill every notification to the MTU.
static void push_transfer_batch()
{
    if (!read_begin_sent) {
        control_notify_buf[0] = NOTIFY_READ_BEGIN;
        put_be64(control_notify_buf + 1, transfer_start_seq);
        put_be32(control_notify_buf + 9, remaining_packets);
        if (!notify_control(control_notify_buf, 13)) {
            reset_transfer();
            return;
        }
        read_begin_sent = true;
    }

    if (remaining_packets == 0U) {
        done_pending = true;
        return;
    }

    const uint32_t packets_to_read =
        (remaining_packets < STORAGE_BATCH_PACKETS) ? remaining_packets : STORAGE_BATCH_PACKETS;
    uint32_t bytes_read = 0;
    uint32_t packets_read = 0;
    const int ret =
        sd_ring_read(current_read_seq, batch_buffer, packets_to_read * RING_RECORD_BYTES, &bytes_read, &packets_read);
    if (ret < 0) {
        transfer_end_status = status_from_ring_error(ret);
        remaining_packets = 0;
        done_pending = true;
        return;
    }
    if (packets_read == 0U || bytes_read == 0U) {
        remaining_packets = 0;
        done_pending = true;
        return;
    }

    const uint16_t chunk = data_chunk_size();
    uint32_t sent = 0;
    while (sent < bytes_read) {
        if (consume_stop()) {
            return;
        }
        const uint32_t payload = ((bytes_read - sent) < chunk) ? (bytes_read - sent) : chunk;
        data_notify_buf[0] = NOTIFY_DATA;
        memcpy(data_notify_buf + 1, batch_buffer + sent, payload);
        if (!notify_control(data_notify_buf, payload + 1U)) {
            reset_transfer();
            return;
        }
        sent += payload;
        // Pace the bulk stream so short control notifications (battery, status)
        // still get a controller buffer.
        vTaskDelay(1);
    }

    current_read_seq += packets_read;
    remaining_packets -= packets_read;
    if (remaining_packets == 0U) {
        done_pending = true;
    }
}

// ---------------------------------------------------------------------------
// BLE callbacks
// ---------------------------------------------------------------------------

class StorageControlCallback : public BLECharacteristicCallbacks
{
    void onWrite(BLECharacteristic *characteristic) override
    {
        const std::string value = characteristic->getValue();
        const size_t len = value.length();
        const uint8_t *bytes = (const uint8_t *) value.data();

        if (len < 1U) {
            queue_ack(STATUS_INVALID_COMMAND);
            return;
        }

        switch (bytes[0]) {
        case CMD_RING_INFO:
            info_requested = true;
            info_deadline_ms = 0;
            latch_sync_intent();
            return;

        case CMD_RING_READ:
            // Length is strict: 9 without a packet count, 13 with one. Anything
            // else means we would be guessing at the caller's intent.
            if (len != 9U && len != 13U) {
                queue_ack(STATUS_INVALID_COMMAND);
                return;
            }
            pending_start_seq = get_be64(bytes + 1);
            pending_packet_count = (len == 13U) ? get_be32(bytes + 9) : 0U;
            read_requested = true;
            read_deadline_ms = 0;
            latch_sync_intent();
            return;

        case CMD_RING_ADVANCE:
            if (len != 9U) {
                queue_ack(STATUS_INVALID_COMMAND);
                return;
            }
            pending_advance_seq = get_be64(bytes + 1);
            advance_requested = true;
            return;

        case CMD_RING_CLEAR:
            clear_requested = true;
            return;

        case CMD_STOP_SYNC:
            stop_requested = true;
            queue_ack(STATUS_OK);
            return;

        default:
            queue_ack(STATUS_INVALID_COMMAND);
            return;
        }
    }
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void storage_service_init(BLEServer *server)
{
    ble_server = server;

    BLEService *service = server->createService(BLEUUID(STORAGE_SERVICE_UUID));

    control_char = service->createCharacteristic(
        BLEUUID(STORAGE_CONTROL_UUID),
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR | BLECharacteristic::PROPERTY_NOTIFY);
    BLE2902 *control_ccc = new BLE2902();
    control_ccc->setNotifications(true);
    control_char->addDescriptor(control_ccc);
    control_char->setCallbacks(new StorageControlCallback());

    status_char = service->createCharacteristic(BLEUUID(STORAGE_STATUS_UUID),
                                                BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
    BLE2902 *status_ccc = new BLE2902();
    status_ccc->setNotifications(true);
    status_char->addDescriptor(status_ccc);

    // The app hides its entire Offline Sync UI when this read comes back short,
    // so seed a valid all-zero snapshot before anything can read it.
    storage_service_refresh_status(true);

    service->start();
}

void storage_service_on_connect(uint16_t conn_id)
{
    peer_conn_id = conn_id;
    peer_connected = true;
    // A fresh session has no sync conversation in progress. Never inherit a latch
    // left behind by a connection that dropped mid-transfer.
    clear_sync_intent();
}

void storage_service_on_disconnect()
{
    peer_connected = false;
    info_requested = false;
    clear_requested = false;
    read_requested = false;
    advance_requested = false;
    stop_requested = false;
    ack_queue_head = ack_queue_tail;
    clear_sync_intent();
    reset_transfer();
}

void storage_service_refresh_status(bool force)
{
    if (status_char == nullptr) {
        return;
    }
    const uint32_t now = millis();
    if (!force && (int32_t) (now - status_refresh_deadline_ms) < 0) {
        return;
    }
    status_refresh_deadline_ms = now + STORAGE_STATUS_REFRESH_MS;

    sd_ring_info_t info = {};
    if (sd_ring_is_ready()) {
        sd_ring_get_info(&info);
    }

    const uint64_t unread = info.write_seq - info.read_seq;
    const uint64_t used = unread * RING_RECORD_BYTES;
    const uint64_t freeb = ((uint64_t) info.capacity_packets - unread) * RING_RECORD_BYTES;

    uint8_t payload[16];
    put_le32(payload + 0, (uint32_t) (used > UINT32_MAX ? UINT32_MAX : used));
    put_le32(payload + 4, (uint32_t) (unread > UINT32_MAX ? UINT32_MAX : unread));
    put_le32(payload + 8, (uint32_t) (freeb > UINT32_MAX ? UINT32_MAX : freeb));
    put_le32(payload + 12, sd_ring_timestamps_authoritative() ? 1U : 0U);
    status_char->setValue(payload, sizeof(payload));
}

bool storage_service_transfer_active()
{
    return transfer_active;
}

bool storage_service_sync_busy()
{
    if (transfer_active || read_requested || info_requested) {
        return true;
    }
    const uint32_t deadline = sync_intent_deadline_ms;
    if (deadline == 0U) {
        return false;
    }
    if ((int32_t) (millis() - deadline) >= 0) {
        sync_intent_deadline_ms = 0;
        return false;
    }
    return true;
}

void storage_service_process()
{
    flush_queued_acks();

    if (consume_stop()) {
        storage_service_refresh_status(true);
    }

    if (info_requested) {
        if (!peer_connected) {
            info_requested = false;
            info_deadline_ms = 0;
        } else if (sd_ring_is_ready()) {
            send_info();
            info_requested = false;
            info_deadline_ms = 0;
        } else if (info_deadline_ms == 0) {
            info_deadline_ms = millis() + STORAGE_SD_READY_TIMEOUT_MS;
        } else if ((int32_t) (millis() - info_deadline_ms) >= 0) {
            send_ack(STATUS_NOT_READY);
            info_requested = false;
            info_deadline_ms = 0;
        }
    }

    if (clear_requested) {
        clear_requested = false;
        clear_sync_intent();
        if (peer_connected) {
            const int ret = sd_ring_clear();
            send_ack(ret < 0 ? status_from_ring_error(ret) : STATUS_OK);
            storage_service_refresh_status(true);
        }
    }

    if (advance_requested) {
        advance_requested = false;
        clear_sync_intent(); // The sync conversation ends here; live drain resumes.
        if (peer_connected) {
            const int ret = sd_ring_advance(pending_advance_seq);
            send_ack(ret < 0 ? status_from_ring_error(ret) : STATUS_OK);
            storage_service_refresh_status(true);
        }
    }

    if (read_requested) {
        if (!peer_connected) {
            read_requested = false;
            read_deadline_ms = 0;
        } else if (sd_ring_is_ready()) {
            start_pending_read();
            read_requested = false;
            read_deadline_ms = 0;
        } else if (read_deadline_ms == 0) {
            read_deadline_ms = millis() + STORAGE_SD_READY_TIMEOUT_MS;
        } else if ((int32_t) (millis() - read_deadline_ms) >= 0) {
            send_ack(STATUS_NOT_READY);
            read_requested = false;
            read_deadline_ms = 0;
        }
    }

    if (transfer_active) {
        if (!peer_connected) {
            reset_transfer();
        } else if (done_pending) {
            // Re-arm rather than clear: the app answers DONE with CMD_RING_ADVANCE
            // quoting the sequence it just received, and a cursor that moves first
            // makes that advance out of range. A long transfer may already have
            // outrun the original latch. Only when DONE actually went out, though
            // — the peer can drop between the check above and here, and re-arming
            // for a conversation nobody is having would suppress live audio into
            // the next connection.
            if (send_done(transfer_end_status, current_read_seq)) {
                latch_sync_intent();
            }
            reset_transfer();
            storage_service_refresh_status(true);
        } else {
            push_transfer_batch();
        }
    } else {
        storage_service_refresh_status(false);
    }
}

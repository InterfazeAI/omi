#include "storage.h"

#include <stdio.h>
#include <string.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/l2cap.h>
#include <zephyr/bluetooth/services/bas.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include "sdcard.h"
#include "transport.h"
#include "utils.h"

LOG_MODULE_REGISTER(storage, CONFIG_LOG_DEFAULT_LEVEL);

#define MAX_PACKET_LENGTH 256
#define OPUS_ENTRY_LENGTH 80
#define FRAME_PREFIX_LENGTH 3

#define READ_COMMAND 0
#define DELETE_COMMAND 1
#define NUKE 2
#define STOP_COMMAND 3

#define INVALID_FILE_SIZE 3
#define ZERO_FILE_SIZE 4
#define INVALID_COMMAND 6
#define END_OF_TRANSFER 100

#define MAX_HEARTBEAT_FRAMES 100
#define HEARTBEAT 50

// Set on a segment number to address that segment's timestamp index rather than its audio.
// A flag rather than a reserved number, because every number is now a real segment.
#define SEGMENT_INDEX_FLAG 0x80
static void storage_config_changed_handler(const struct bt_gatt_attr *attr, uint16_t value);
static ssize_t storage_write_handler(struct bt_conn *conn,
                                     const struct bt_gatt_attr *attr,
                                     const void *buf,
                                     uint16_t len,
                                     uint16_t offset,
                                     uint8_t flags);

static struct bt_uuid_128 storage_service_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x30295780, 0x4301, 0xEABD, 0x2904, 0x2849ADFEAE43));
static struct bt_uuid_128 storage_write_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x30295781, 0x4301, 0xEABD, 0x2904, 0x2849ADFEAE43));
static struct bt_uuid_128 storage_read_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x30295782, 0x4301, 0xEABD, 0x2904, 0x2849ADFEAE43));
static ssize_t storage_read_characteristic(struct bt_conn *conn,
                                           const struct bt_gatt_attr *attr,
                                           void *buf,
                                           uint16_t len,
                                           uint16_t offset);

K_THREAD_STACK_DEFINE(storage_stack, 4096);
static struct k_thread storage_thread;

extern uint32_t file_num_array[2];
void broadcast_storage_packet(struct k_work *work_item);

// Layout of the storage-info characteristic. See storage_read_characteristic().
#define STORAGE_INFO_BYTES 34

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}

static struct bt_gatt_attr storage_service_attr[] = {
    BT_GATT_PRIMARY_SERVICE(&storage_service_uuid),
    BT_GATT_CHARACTERISTIC(&storage_write_uuid.uuid,
                           BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_WRITE,
                           NULL,
                           storage_write_handler,
                           NULL),
    BT_GATT_CCC(storage_config_changed_handler, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    BT_GATT_CHARACTERISTIC(&storage_read_uuid.uuid,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ,
                           storage_read_characteristic,
                           NULL,
                           NULL),
    BT_GATT_CCC(storage_config_changed_handler, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

};

struct bt_gatt_service storage_service = BT_GATT_SERVICE(storage_service_attr);

static void storage_config_changed_handler(const struct bt_gatt_attr *attr, uint16_t value)
{
    if (value == BT_GATT_CCC_NOTIFY) {
        LOG_INF("Client subscribed for notifications");
    } else if (value == 0) {
        LOG_INF("Client unsubscribed from notifications");
    } else {
        LOG_ERR("Invalid CCC value: %u", value);
    }
}

static ssize_t storage_read_characteristic(struct bt_conn *conn,
                                           const struct bt_gatt_attr *attr,
                                           void *buf,
                                           uint16_t len,
                                           uint16_t offset)
{
    k_msleep(10);

    // The first two words keep their original meaning so existing clients that read only 8
    // bytes still work; the ring geometry is appended after them.
    uint8_t info[STORAGE_INFO_BYTES];
    put_le32(info + 0, file_num_array[0]); // bytes in the segment being recorded
    put_le32(info + 4, file_num_array[1]); // last read offset the device saved
    info[8] = storage_segment_count();
    put_le32(info + 9, storage_segment_oldest_seq());
    put_le32(info + 13, storage_segment_newest_seq());
    put_le32(info + 17, storage_segment_target_bytes());

    uint32_t evictions = 0, sync_errors = 0;
    int32_t last_evict_err = 0;
    storage_ring_stats(&evictions, &last_evict_err, &sync_errors);
    info[21] = (uint8_t) storage_segment_max_count();
    put_le32(info + 22, evictions);
    put_le32(info + 26, (uint32_t) last_evict_err);
    put_le32(info + 30, sync_errors);

    return bt_gatt_attr_read(conn, attr, buf, len, offset, info, sizeof(info));
}

uint8_t transport_started = 0;

static uint16_t packet_next_index = 0;
#define SD_BLE_SIZE 440
static uint8_t storage_write_buffer[SD_BLE_SIZE];

static uint32_t offset = 0;
static uint8_t index = 0;
static uint8_t current_packet_size = 0;
static uint8_t tx_buffer_size = 0;
static uint8_t stop_started = 0;
static uint8_t delete_started = 0;
static uint8_t current_read_num = 1;
static bool current_read_is_index = false;
uint32_t remaining_length = 0;

static int setup_storage_tx()
{
    transport_started = (uint8_t) 0;
    LOG_INF("about to transmit storage\n");
    k_msleep(1000);

    // Recording continues during a sync, so commit the in-flight block before measuring:
    // the client should be able to pull everything captured up to this moment.
    storage_flush();

    uint32_t total;

    int res =
        storage_select_read_target(current_read_num, current_read_is_index ? STORAGE_READ_INDEX : STORAGE_READ_AUDIO);
    if (res) {
        LOG_INF("bad pointer");
        transport_started = 0;
        current_read_num = 1;
        current_read_is_index = false;
        remaining_length = 0;
        return -1;
    }

    if (current_read_is_index) {
        total = index_get_size_for(current_read_num);
        LOG_INF("serving timestamp index of segment %u, %u bytes", current_read_num, total);
    } else {
        // Always ask the storage layer: only it knows that the newest segment's length lives
        // in a counter rather than the directory entry.
        total = get_file_size(current_read_num);
        LOG_INF("serving segment %u, %u bytes", current_read_num, total);
    }

    // Both of these used to be answered synchronously by the write handler; they moved here so
    // that reading the size cannot run on the BT RX stack. The client is told directly, because
    // the transfer loop below only speaks when it has bytes to send.
    if (total == 0 || offset >= total) {
        uint8_t code[1] = {total == 0 ? ZERO_FILE_SIZE : END_OF_TRANSFER};
        LOG_INF("nothing to send from segment %u: offset %u of %u", current_read_num, offset, total);
        struct bt_conn *conn = get_current_connection();
        if (conn) {
            bt_gatt_notify(conn, &storage_service.attrs[1], code, 1);
        }
        remaining_length = 0;
        transport_started = 0;
        current_read_is_index = false;
        return 0;
    }

    remaining_length = total - offset;

    LOG_INF("remaining length: %d", remaining_length);
    LOG_INF("offset: %d", offset);
    LOG_INF("file: %d", current_read_num);

    return 0;
}
uint8_t delete_num = 0;
uint8_t nuke_started = 0;
static uint8_t heartbeat_count = 0;
static uint8_t parse_storage_command(void *buf, uint16_t len)
{

    if (len != 6 && len != 2) {
        LOG_INF("invalid command");
        return INVALID_COMMAND;
    }
    const uint8_t command = ((uint8_t *) buf)[0];
    const uint8_t file_num = ((uint8_t *) buf)[1];
    uint32_t size = 0;
    if (len == 6) {
        size =
            ((uint8_t *) buf)[2] << 24 | ((uint8_t *) buf)[3] << 16 | ((uint8_t *) buf)[4] << 8 | ((uint8_t *) buf)[5];
    }
    LOG_PRINTK("command successful: command: %d file: %d size: %d \n", command, file_num, size);

    // The timestamp index of a segment is addressed by setting the high bit of its number, so
    // clients fetch it over the existing transfer path instead of needing a second protocol.
    const bool want_index = (file_num & SEGMENT_INDEX_FLAG) != 0;
    const uint8_t seg_num = file_num & ~SEGMENT_INDEX_FLAG;

    if (seg_num == 0 || seg_num > storage_segment_count()) {
        LOG_INF("invalid segment %u of %u", seg_num, storage_segment_count());
        return INVALID_FILE_SIZE;
    }

    if (want_index && command != READ_COMMAND) {
        LOG_INF("only reads are supported for the index");
        return INVALID_COMMAND;
    }

    if (command == READ_COMMAND) // read
    {
        // Deliberately no filesystem call here. This runs on the BT RX thread, which has a
        // 1 KB stack (CONFIG_BT_RX_STACK_SIZE); a FatFs directory walk does not fit and faults
        // the device. It went unnoticed while there was only ever one file, because
        // get_file_size() answers for the segment being recorded from a counter and never
        // touches the card -- every other target does. setup_storage_tx() now decides the size
        // on the storage thread and reports an empty or exhausted target from there.
        offset = size - (size % SD_BLE_SIZE); // round down to nearest SD_BLE_SIZE
        current_read_num = seg_num;
        current_read_is_index = want_index;
        transport_started = 1;
    } else if (command == DELETE_COMMAND) {
        delete_num = seg_num;
        delete_started = 1;
    } else if (command == NUKE) {
        nuke_started = 1;
    } else if (command == STOP_COMMAND) // should be no explicit stop command, send heartbeats to keep connection alive
    {
        remaining_length = 0;
        stop_started = 1;
    } else if (command == HEARTBEAT) {
        heartbeat_count = 0;
    } else {
        LOG_INF("invalid command \n");
        return 6;
    }
    return 0;
}

static ssize_t storage_write_handler(struct bt_conn *conn,
                                     const struct bt_gatt_attr *attr,
                                     const void *buf,
                                     uint16_t len,
                                     uint16_t offset,
                                     uint8_t flags)
{
    LOG_INF("about to schedule the storage");
    LOG_INF("was sent %d  ", ((uint8_t *) buf)[0]);

    uint8_t result_buffer[1] = {0};
    uint8_t result = parse_storage_command(buf, len);
    result_buffer[0] = result;
    LOG_INF("length of storage write: %d", len);
    LOG_INF("result: %d ", result);
    bt_gatt_notify(conn, &storage_service.attrs[1], &result_buffer, 1);
    k_msleep(500);
    return len;
}

// static void write_to_gatt(struct bt_conn *conn)
// {
//     uint32_t id = packet_next_index++;
//     index = 0;
//     storage_write_buffer[0] = id & 0xFF;
//     storage_write_buffer[1] = (id >> 8) & 0xFF;
//     storage_write_buffer[2] = index;

//     const uint32_t packet_size = MIN(remaining_length,OPUS_ENTRY_LENGTH);

//     int r = read_audio_data(storage_write_buffer+FRAME_PREFIX_LENGTH,packet_size,offset);
//     offset = offset + packet_size;

//     index++;

//     int err = bt_gatt_notify(conn, &storage_service.attrs[1], &storage_write_buffer,packet_size+FRAME_PREFIX_LENGTH);
//     if (err)
//     {
//         LOG_PRINTK("error writing to gatt: %d\n",err);
//     }
//     else
//     {
//     remaining_length = remaining_length - OPUS_ENTRY_LENGTH;
//     }
// }

static void write_to_gatt(struct bt_conn *conn)
{ // unsafe. designed for max speeds. udp?

    uint32_t packet_size = MIN(remaining_length, SD_BLE_SIZE);

    int r = read_audio_data(storage_write_buffer, packet_size, offset);
    if (r <= 0) {
        LOG_ERR("storage read failed at offset %u: %d", offset, r);
        remaining_length = 0;
        return;
    }
    packet_size = (uint32_t) r;

    int err = bt_gatt_notify(conn, &storage_service.attrs[1], &storage_write_buffer, packet_size);
    if (err) {
        LOG_PRINTK("error writing to gatt: %d\n", err);
        return; // leave offset and remaining_length alone so the block is retried
    }

    // Advancing by the actual packet size matters on the final block: subtracting a full
    // SD_BLE_SIZE from a shorter tail wrapped remaining_length around to a huge value.
    offset += packet_size;
    remaining_length -= MIN(remaining_length, packet_size);
}

void storage_write(void)
{
    while (1) {
        struct bt_conn *conn = get_current_connection();

        if (transport_started) {
            LOG_INF("transpor started in side : %d", transport_started);
            setup_storage_tx();
        }
        // probably prefer to implement using work orders for delete,nuke,etc...
        if (delete_started) {
            LOG_INF("deleting segment %d", delete_num);
            int err = clear_audio_file(delete_num);
            offset = 0;
            save_offset(offset);

            if (err) {
                LOG_PRINTK("error clearing\n");
            } else {
                uint8_t result_buffer[1] = {200};
                if (conn) {
                    bt_gatt_notify(get_current_connection(), &storage_service.attrs[1], &result_buffer, 1);
                }
            }
            delete_started = 0;
            k_msleep(10);
        }
        if (nuke_started) {
            clear_audio_directory();
            save_offset(0);
            nuke_started = 0;
        }
        // Time sync arrives on the BT RX thread, which cannot write the record itself.
        storage_index_service();
        if (stop_started) {
            remaining_length = 0;
            stop_started = 0;
            save_offset(offset);
            storage_read_close();
        }
        if (heartbeat_count == MAX_HEARTBEAT_FRAMES) {
            LOG_PRINTK("no heartbeat sent\n");
            save_offset(offset);
            // k_yield();
            // continue;
        }

        if (remaining_length > 0) {
            if (conn == NULL) {
                LOG_ERR("invalid connection");
                remaining_length = 0;
                save_offset(offset);
                storage_read_close();
                continue;
            }
            // LOG_PRINTK("remaining length: %d\n",remaining_length);

            write_to_gatt(conn);
            heartbeat_count = (heartbeat_count + 1) % (MAX_HEARTBEAT_FRAMES + 1);

            transport_started = 0;
            if (remaining_length == 0) {
                storage_read_close();
                if (stop_started) {
                    stop_started = 0;
                } else {
                    LOG_PRINTK("done. attempting to download more files\n");
                    uint8_t stop_result[1] = {END_OF_TRANSFER};
                    int err = bt_gatt_notify(get_current_connection(), &storage_service.attrs[1], &stop_result, 1);
                    k_sleep(K_MSEC(10));
                }
            }
        }
        k_yield();
    }
}

int storage_init()
{
    k_thread_create(&storage_thread,
                    storage_stack,
                    K_THREAD_STACK_SIZEOF(storage_stack),
                    (k_thread_entry_t) storage_write,
                    NULL,
                    NULL,
                    NULL,
                    K_PRIO_PREEMPT(7),
                    0,
                    K_NO_WAIT);
    return 0;
}

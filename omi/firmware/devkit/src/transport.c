#include "transport.h"

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/l2cap.h>
#include <zephyr/bluetooth/services/bas.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/ring_buffer.h>

#include "config.h"
#include "utils.h"
// #include "nfc.h"
#include "button.h"
#include "lib/battery/battery.h"
#include "mic.h"
#include "rtc.h"
#include "sdcard.h"
#include "speaker.h"
#include "storage.h"
// #include "friend.h"
LOG_MODULE_REGISTER(transport, CONFIG_LOG_DEFAULT_LEVEL);

extern bool is_connected;
extern uint32_t file_num_array[2];
struct bt_conn *current_connection = NULL;
uint16_t current_mtu = 0;
uint16_t current_package_index = 0;

//
// Internal
//

struct k_mutex write_sdcard_mutex;

// Only ever referenced from a LOG_DBG, so at the shipped log level the call is compiled out and
// the missing definition costs nothing -- which is why this went unnoticed. Raise the log level
// far enough to keep LOG_DBG and the image stops linking, so every debug build was broken.
static const char *phy2str(uint8_t phy)
{
    switch (phy) {
    case BT_GAP_LE_PHY_NONE:
        return "none";
    case BT_GAP_LE_PHY_1M:
        return "LE 1M";
    case BT_GAP_LE_PHY_2M:
        return "LE 2M";
    case BT_GAP_LE_PHY_CODED:
        return "LE Coded";
    default:
        return "unknown";
    }
}

static ssize_t audio_data_write_handler(struct bt_conn *conn,
                                        const struct bt_gatt_attr *attr,
                                        const void *buf,
                                        uint16_t len,
                                        uint16_t offset,
                                        uint8_t flags);

static struct bt_conn_cb _callback_references;
static void audio_ccc_config_changed_handler(const struct bt_gatt_attr *attr, uint16_t value);
static ssize_t audio_data_read_characteristic(struct bt_conn *conn,
                                              const struct bt_gatt_attr *attr,
                                              void *buf,
                                              uint16_t len,
                                              uint16_t offset);
static ssize_t audio_codec_read_characteristic(struct bt_conn *conn,
                                               const struct bt_gatt_attr *attr,
                                               void *buf,
                                               uint16_t len,
                                               uint16_t offset);

static void dfu_ccc_config_changed_handler(const struct bt_gatt_attr *attr, uint16_t value);
static ssize_t dfu_control_point_write_handler(struct bt_conn *conn,
                                               const struct bt_gatt_attr *attr,
                                               const void *buf,
                                               uint16_t len,
                                               uint16_t offset,
                                               uint8_t flags);

//
// Service and Characteristic
//
// Audio service with UUID 19B10000-E8F2-537E-4F6C-D104768A1214
// exposes following characteristics:
// - Audio data (UUID 19B10001-E8F2-537E-4F6C-D104768A1214) to send audio data (read/notify)
// - Audio codec (UUID 19B10002-E8F2-537E-4F6C-D104768A1214) to send audio codec type (read)
// TODO: The current audio service UUID seems to come from old Intel sample code,
// we should change it to UUID 814b9b7c-25fd-4acd-8604-d28877beee6d
static struct bt_uuid_128 audio_service_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x19B10000, 0xE8F2, 0x537E, 0x4F6C, 0xD104768A1214));
static struct bt_uuid_128 audio_characteristic_data_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x19B10001, 0xE8F2, 0x537E, 0x4F6C, 0xD104768A1214));
static struct bt_uuid_128 audio_characteristic_format_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x19B10002, 0xE8F2, 0x537E, 0x4F6C, 0xD104768A1214));
static struct bt_uuid_128 audio_characteristic_speaker_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x19B10003, 0xE8F2, 0x537E, 0x4F6C, 0xD104768A1214));

// Every permission below is the _ENCRYPT variant: live microphone audio and the recording it
// belongs to must not be readable by an unpaired device. The CCC reads stay unencrypted because
// the subscription flag itself reveals nothing -- the write that starts the stream is what
// matters, and that is gated.
static struct bt_gatt_attr audio_service_attr[] = {
    BT_GATT_PRIMARY_SERVICE(&audio_service_uuid),
    BT_GATT_CHARACTERISTIC(&audio_characteristic_data_uuid.uuid,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           OMI_PERM_READ,
                           audio_data_read_characteristic,
                           NULL,
                           NULL),
    BT_GATT_CCC(audio_ccc_config_changed_handler, BT_GATT_PERM_READ | OMI_PERM_WRITE),
    BT_GATT_CHARACTERISTIC(&audio_characteristic_format_uuid.uuid,
                           BT_GATT_CHRC_READ,
                           OMI_PERM_READ,
                           audio_codec_read_characteristic,
                           NULL,
                           NULL),
#ifdef CONFIG_OMI_ENABLE_SPEAKER
    BT_GATT_CHARACTERISTIC(&audio_characteristic_speaker_uuid.uuid,
                           BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                           OMI_PERM_WRITE,
                           NULL,
                           audio_data_write_handler,
                           NULL),
    BT_GATT_CCC(audio_ccc_config_changed_handler, BT_GATT_PERM_READ | OMI_PERM_WRITE), //
#endif

};

static struct bt_gatt_service audio_service = BT_GATT_SERVICE(audio_service_attr);

// Time Sync service with UUID 19B10030-E8F2-537E-4F6C-D104768A1214
// exposes following characteristics:
// - Time write (UUID 19B10031-...) accepts 4 bytes of UTC epoch seconds to set the clock
// - Time read  (UUID 19B10032-...) returns the device's current epoch, 0 if unsynchronized
// UUIDs deliberately match the newer omi firmware so the existing app can sync this device.
static struct bt_uuid_128 time_sync_service_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x19B10030, 0xE8F2, 0x537E, 0x4F6C, 0xD104768A1214));
static struct bt_uuid_128 time_sync_write_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x19B10031, 0xE8F2, 0x537E, 0x4F6C, 0xD104768A1214));
static struct bt_uuid_128 time_sync_read_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x19B10032, 0xE8F2, 0x537E, 0x4F6C, 0xD104768A1214));

static ssize_t time_sync_write_handler(struct bt_conn *conn,
                                       const struct bt_gatt_attr *attr,
                                       const void *buf,
                                       uint16_t len,
                                       uint16_t offset,
                                       uint8_t flags)
{
    if (len != sizeof(uint32_t)) {
        LOG_WRN("Invalid length for time sync write: %u", len);
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    uint32_t epoch_s;
    memcpy(&epoch_s, buf, sizeof(epoch_s));

    int err = rtc_set_epoch(epoch_s);
    if (err) {
        LOG_ERR("Failed to set clock: %d", err);
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }

    return len;
}

static ssize_t
time_sync_read_handler(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf, uint16_t len, uint16_t offset)
{
    uint32_t epoch_s = rtc_get_epoch();
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &epoch_s, sizeof(epoch_s));
}

// The clock is gated too: it stamps the recording index, so an unpaired writer could misdate
// every future segment and make a recording impossible to place in time.
static struct bt_gatt_attr time_sync_service_attr[] = {
    BT_GATT_PRIMARY_SERVICE(&time_sync_service_uuid),
    BT_GATT_CHARACTERISTIC(&time_sync_write_uuid.uuid,
                           BT_GATT_CHRC_WRITE,
                           OMI_PERM_WRITE,
                           NULL,
                           time_sync_write_handler,
                           NULL),
    BT_GATT_CHARACTERISTIC(&time_sync_read_uuid.uuid,
                           BT_GATT_CHRC_READ,
                           OMI_PERM_READ,
                           time_sync_read_handler,
                           NULL,
                           NULL),
};

static struct bt_gatt_service time_sync_service = BT_GATT_SERVICE(time_sync_service_attr);

// Nordic Legacy DFU service with UUID 00001530-1212-EFDE-1523-785FEABCD123
// exposes following characteristics:
// - Control point (UUID 00001531-1212-EFDE-1523-785FEABCD123) to start the OTA update process (write/notify)
static struct bt_uuid_128 dfu_service_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x00001530, 0x1212, 0xEFDE, 0x1523, 0x785FEABCD123));
static struct bt_uuid_128 dfu_control_point_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x00001531, 0x1212, 0xEFDE, 0x1523, 0x785FEABCD123));

// One unauthenticated byte here used to reboot the board into the bootloader from anywhere in
// radio range, which stops recording until someone power-cycles it by hand.
static struct bt_gatt_attr dfu_service_attr[] = {
    BT_GATT_PRIMARY_SERVICE(&dfu_service_uuid),
    BT_GATT_CHARACTERISTIC(&dfu_control_point_uuid.uuid,
                           BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
                           OMI_PERM_WRITE,
                           NULL,
                           dfu_control_point_write_handler,
                           NULL),
    BT_GATT_CCC(dfu_ccc_config_changed_handler, BT_GATT_PERM_READ | OMI_PERM_WRITE),
};

static struct bt_gatt_service dfu_service = BT_GATT_SERVICE(dfu_service_attr);

//
// Pairing service with UUID 19B10040-E8F2-537E-4F6C-D104768A1214
// - Status  (19B10041-...) read, DELIBERATELY UNENCRYPTED
// - Release (19B10042-...) write, encrypted, releases the bond slot
//
// The status characteristic must stay readable without pairing. It exists to explain why pairing
// did not work, and a diagnostic that requires the thing it diagnoses is useless. It publishes no
// secret: counters, a bond count and an error code. Everything that exposes actual content --
// audio, recordings, the clock, DFU -- stays gated.
//
// It is also the only usable channel. Serial logging on this board is not reliable (see
// DEBUGGING.md trap 8) and the SMP debug image does not boot, so without this the failure reason
// the stack already knows is simply thrown away.
static struct bt_uuid_128 pairing_service_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x19B10040, 0xE8F2, 0x537E, 0x4F6C, 0xD104768A1214));
static struct bt_uuid_128 pairing_status_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x19B10041, 0xE8F2, 0x537E, 0x4F6C, 0xD104768A1214));
static struct bt_uuid_128 pairing_release_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x19B10042, 0xE8F2, 0x537E, 0x4F6C, 0xD104768A1214));

#define PAIRING_STATUS_BYTES 25

// Any write to the release characteristic must carry exactly this. The encrypted link already
// proves the writer is the owner; the magic guards against a buggy or truncated write silently
// throwing away the bond and locking the owner out of their own device.
static const uint8_t unbond_magic[8] = {'O', 'M', 'I', 'U', 'N', 'B', 'N', 'D'};

static uint32_t connection_count = 0;
static uint32_t pairing_successes = 0;
static uint32_t pairing_failures = 0;
static uint32_t unbond_requests = 0;
static uint8_t last_pairing_err = 0;
static uint8_t last_security_err = 0;
static uint8_t last_security_level = 0;
static int8_t last_unbond_result = 0;

static void bond_count_cb(const struct bt_bond_info *info, void *user_data)
{
    (*(uint8_t *) user_data)++;
}

static uint8_t count_bonds(void)
{
    uint8_t n = 0;
#if defined(CONFIG_BT_SMP)
    // Walks the in-RAM key table, not flash, so this is safe from a GATT handler. Anything that
    // reached storage would violate DEBUGGING.md trap 7.
    bt_foreach_bond(BT_ID_DEFAULT, bond_count_cb, &n);
#endif
    return n;
}

static ssize_t pairing_status_read_handler(struct bt_conn *conn,
                                           const struct bt_gatt_attr *attr,
                                           void *buf,
                                           uint16_t len,
                                           uint16_t offset)
{
    uint8_t status[PAIRING_STATUS_BYTES] = {0};

    uint8_t flags = 0;
    if (IS_ENABLED(CONFIG_BT_SMP)) {
        flags |= 0x01;
    }
    if (IS_ENABLED(CONFIG_BT_BONDABLE)) {
        flags |= 0x02;
    }
    if (IS_ENABLED(CONFIG_BT_SETTINGS)) {
        flags |= 0x04;
    }
    if (conn && bt_conn_get_security(conn) >= BT_SECURITY_L2) {
        flags |= 0x08;
    }

    status[0] = 1; // layout version, so a host tool can refuse a payload it cannot read
    status[1] = flags;
    status[2] = count_bonds();
    status[3] = CONFIG_BT_MAX_PAIRED;
    status[4] = last_pairing_err;
    status[5] = last_security_err;
    status[6] = last_security_level;
    status[7] = conn ? (uint8_t) bt_conn_get_security(conn) : 0;
    sys_put_le32(connection_count, status + 8);
    sys_put_le32(pairing_successes, status + 12);
    sys_put_le32(pairing_failures, status + 16);
    sys_put_le32(unbond_requests, status + 20);
    status[24] = (uint8_t) last_unbond_result;

    return bt_gatt_attr_read(conn, attr, buf, len, offset, status, sizeof(status));
}

// bt_unpair() writes the bond out of NVS. That is flash work, and a GATT handler runs on the
// Bluetooth RX thread with a 1 KB stack -- the exact shape that crashed this firmware twice before
// (DEBUGGING.md trap 7). The handler only latches; the system work queue does the erase.
static void unbond_work_handler(struct k_work *work)
{
    int err = bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY);
    last_unbond_result = (int8_t) err;
    LOG_WRN("bond slot released by owner request (err %d)", err);

    // The peer keeps its half of a bond that no longer exists here, so leaving it connected means
    // an encrypted link backed by a key the device has forgotten. Drop it and let it reconnect.
    if (current_connection) {
        bt_conn_disconnect(current_connection, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
}

static K_WORK_DEFINE(unbond_work, unbond_work_handler);

// Called by the storage thread once the card has been erased. Kept as a separate step so the
// bond outlives the wipe: an interrupted release leaves the device still bonded rather than
// leaving an unwiped card open to whoever pairs next.
void transport_finish_unbond(void)
{
    k_work_submit(&unbond_work);
}

static ssize_t pairing_release_write_handler(struct bt_conn *conn,
                                             const struct bt_gatt_attr *attr,
                                             const void *buf,
                                             uint16_t len,
                                             uint16_t offset,
                                             uint8_t flags)
{
    if (len != sizeof(unbond_magic) || memcmp(buf, unbond_magic, sizeof(unbond_magic)) != 0) {
        LOG_WRN("rejected bond release: bad magic (len %u)", len);
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }

    unbond_requests++;

    // Does not unpair here. Releasing the bond must destroy the recordings first, or the next
    // device to pair inherits the previous owner's audio. The storage thread erases the card and
    // then calls transport_finish_unbond(); this handler only asks.
    storage_request_unbond_wipe();
    return len;
}

static struct bt_gatt_attr pairing_service_attr[] = {
    BT_GATT_PRIMARY_SERVICE(&pairing_service_uuid),
    BT_GATT_CHARACTERISTIC(&pairing_status_uuid.uuid,
                           BT_GATT_CHRC_READ,
                           BT_GATT_PERM_READ,
                           pairing_status_read_handler,
                           NULL,
                           NULL),
    // Encrypted on purpose: only the device that currently holds the bond may give it up. An
    // unpaired attacker cannot reach this for the same reason it cannot read the recordings.
    BT_GATT_CHARACTERISTIC(&pairing_release_uuid.uuid,
                           BT_GATT_CHRC_WRITE,
                           OMI_PERM_WRITE,
                           NULL,
                           pairing_release_write_handler,
                           NULL),
};

static struct bt_gatt_service pairing_service = BT_GATT_SERVICE(pairing_service_attr);

// Diagnostics service with UUID 19B10050-E8F2-537E-4F6C-D104768A1214
// - Battery (19B10051-...) read, unencrypted
//
// The standard Battery Service reports one number, and when that number is wrong it cannot say
// why. A floating sense input reports 0% or 100% purely according to the sign of the noise, which
// looks like a flat or a full battery rather than like no measurement at all. This exposes the
// inputs behind the number so the difference is visible from the host.
static struct bt_uuid_128 diagnostics_service_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x19B10050, 0xE8F2, 0x537E, 0x4F6C, 0xD104768A1214));
static struct bt_uuid_128 battery_diag_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x19B10051, 0xE8F2, 0x537E, 0x4F6C, 0xD104768A1214));

#define BATTERY_DIAG_BYTES 26

static ssize_t battery_diag_read_handler(struct bt_conn *conn,
                                         const struct bt_gatt_attr *attr,
                                         void *buf,
                                         uint16_t len,
                                         uint16_t offset)
{
    struct battery_diag diag = {0};
    uint8_t out[BATTERY_DIAG_BYTES] = {0};

    int err = battery_get_diagnostics(&diag);

    out[0] = 5; // layout version
    out[1] = diag.read_enable;
    sys_put_le16((uint16_t) diag.raw_counts, &out[2]);
    sys_put_le32((uint32_t) diag.adc_mv, &out[4]);
    sys_put_le32((uint32_t) diag.battery_mv, &out[8]);
    out[12] = diag.percentage;
    out[13] = (uint8_t) diag.init_err;
    out[14] = (uint8_t) diag.setup_err;
    out[15] = (uint8_t) diag.gpio_err;
    out[16] = (uint8_t) diag.read_err;
    out[17] = (uint8_t) err;
    sys_put_le16((uint16_t) diag.off_counts, &out[18]);
    out[20] = diag.enable_is_output;
    out[21] = diag.charging;
    sys_put_le32((uint32_t) diag.vdd_mv, &out[22]);

    return bt_gatt_attr_read(conn, attr, buf, len, offset, out, sizeof(out));
}

static struct bt_gatt_attr diagnostics_service_attr[] = {
    BT_GATT_PRIMARY_SERVICE(&diagnostics_service_uuid),
    BT_GATT_CHARACTERISTIC(&battery_diag_uuid.uuid,
                           BT_GATT_CHRC_READ,
                           BT_GATT_PERM_READ,
                           battery_diag_read_handler,
                           NULL,
                           NULL),
};

static struct bt_gatt_service diagnostics_service = BT_GATT_SERVICE(diagnostics_service_attr);

#if defined(CONFIG_BT_SMP)
static void pairing_complete_cb(struct bt_conn *conn, bool bonded)
{
    pairing_successes++;
    last_pairing_err = 0;
    LOG_INF("pairing complete (bonded %d), bonds now %u", bonded, count_bonds());
}

static void pairing_failed_cb(struct bt_conn *conn, enum bt_security_err reason)
{
    pairing_failures++;
    last_pairing_err = (uint8_t) reason;
    LOG_ERR("pairing failed, reason %u", reason);
}

static struct bt_conn_auth_info_cb auth_info_callbacks = {
    .pairing_complete = pairing_complete_cb,
    .pairing_failed = pairing_failed_cb,
};
#endif

// Acceleration data
// this code activates the onboard accelerometer. some cute ideas may include shaking the necklace to color strobe
//
static struct sensors mega_sensor;
static struct device *lsm6dsl_dev;
// Arbritrary uuid, feel free to change
static struct bt_uuid_128 accel_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x32403790, 0x0000, 0x1000, 0x7450, 0xBF445E5829A2));
static struct bt_uuid_128 accel_uuid_x =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x32403791, 0x0000, 0x1000, 0x7450, 0xBF445E5829A2));

static void accel_ccc_config_changed_handler(const struct bt_gatt_attr *attr, uint16_t value);
static ssize_t accel_data_read_characteristic(struct bt_conn *conn,
                                              const struct bt_gatt_attr *attr,
                                              void *buf,
                                              uint16_t len,
                                              uint16_t offset);

static struct bt_gatt_attr accel_service_attr[] = {
    BT_GATT_PRIMARY_SERVICE(&accel_uuid), // primary description
    BT_GATT_CHARACTERISTIC(&accel_uuid_x.uuid,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ,
                           accel_data_read_characteristic,
                           NULL,
                           NULL),                                                          // data type
    BT_GATT_CCC(accel_ccc_config_changed_handler, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE), // scheduler
};
static struct bt_gatt_service accel_service = BT_GATT_SERVICE(accel_service_attr);

static ssize_t accel_data_read_characteristic(struct bt_conn *conn,
                                              const struct bt_gatt_attr *attr,
                                              void *buf,
                                              uint16_t len,
                                              uint16_t offset)
{
    LOG_INF("Acceleration data read characteristic");
    int axis_mode = 6; // 3 for accel, 6 for (also) gyro
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &axis_mode, sizeof(axis_mode));
}

#define ACCEL_REFRESH_INTERVAL 1000 // 1.0 seconds

void broadcast_accel(struct k_work *work_item);
K_WORK_DELAYABLE_DEFINE(accel_work, broadcast_accel);

void broadcast_accel(struct k_work *work_item)
{

    sensor_sample_fetch_chan(lsm6dsl_dev, SENSOR_CHAN_ACCEL_XYZ);
    sensor_channel_get(lsm6dsl_dev, SENSOR_CHAN_ACCEL_X, &mega_sensor.a_x);
    sensor_channel_get(lsm6dsl_dev, SENSOR_CHAN_ACCEL_Y, &mega_sensor.a_y);
    sensor_channel_get(lsm6dsl_dev, SENSOR_CHAN_ACCEL_Z, &mega_sensor.a_z);

    sensor_sample_fetch_chan(lsm6dsl_dev, SENSOR_CHAN_GYRO_XYZ);
    sensor_channel_get(lsm6dsl_dev, SENSOR_CHAN_GYRO_X, &mega_sensor.g_x);
    sensor_channel_get(lsm6dsl_dev, SENSOR_CHAN_GYRO_Y, &mega_sensor.g_y);
    sensor_channel_get(lsm6dsl_dev, SENSOR_CHAN_GYRO_Z, &mega_sensor.g_z);

    // only time mega sensor is changed is through here (hopefully),  so no chance of race condition
    int err = bt_gatt_notify(current_connection, &accel_service.attrs[1], &mega_sensor, sizeof(mega_sensor));
    if (err) {
        LOG_ERR("Error updating Accelerometer data");
    }
    k_work_reschedule(&accel_work, K_MSEC(ACCEL_REFRESH_INTERVAL));
}

struct gpio_dt_spec accel_gpio_pin = {.port = DEVICE_DT_GET(DT_NODELABEL(gpio1)),
                                      .pin = 8,
                                      .dt_flags = GPIO_INT_DISABLE};

// use d4,d5
static void accel_ccc_config_changed_handler(const struct bt_gatt_attr *attr, uint16_t value)
{
    if (value == BT_GATT_CCC_NOTIFY) {
        LOG_INF("Client subscribed for notifications");
    } else if (value == 0) {
        LOG_INF("Client unsubscribed from notifications");
    } else {
        LOG_ERR("Invalid CCC value: %u", value);
    }
}

int accel_start()
{
    struct sensor_value odr_attr;
    lsm6dsl_dev = DEVICE_DT_GET_ONE(st_lsm6dsl);
    k_msleep(50);
    if (lsm6dsl_dev == NULL) {
        LOG_ERR("Could not get LSM6DSL device");
        return 0;
    }
    if (!device_is_ready(lsm6dsl_dev)) {
        LOG_ERR("LSM6DSL: not ready");
        return 0;
    }
    odr_attr.val1 = 10;
    odr_attr.val2 = 0;

    if (gpio_is_ready_dt(&accel_gpio_pin)) {
        LOG_PRINTK("Speaker Pin ready\n");
    } else {
        LOG_PRINTK("Error setting up speaker Pin\n");
        return -1;
    }
    if (gpio_pin_configure_dt(&accel_gpio_pin, GPIO_OUTPUT_INACTIVE) < 0) {
        LOG_PRINTK("Error setting up Haptic Pin\n");
        return -1;
    }
    gpio_pin_set_dt(&accel_gpio_pin, 1);
    if (sensor_attr_set(lsm6dsl_dev, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_SAMPLING_FREQUENCY, &odr_attr) < 0) {
        LOG_ERR("Cannot set sampling frequency for Accelerometer.");
        return 0;
    }
    if (sensor_attr_set(lsm6dsl_dev, SENSOR_CHAN_GYRO_XYZ, SENSOR_ATTR_SAMPLING_FREQUENCY, &odr_attr) < 0) {
        LOG_ERR("Cannot set sampling frequency for gyro.");
        return 0;
    }
    if (sensor_sample_fetch(lsm6dsl_dev) < 0) {
        LOG_ERR("Sensor sample update error");
        return 0;
    }

    LOG_INF("Accelerometer is ready for use \n");

    return 1;
}
// Advertisement data
static const struct bt_data bt_ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA(BT_DATA_UUID128_ALL, audio_service_uuid.val, sizeof(audio_service_uuid.val)),
    BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

// Scan response data
static const struct bt_data bt_sd[] = {
    BT_DATA_BYTES(BT_DATA_UUID16_ALL, BT_UUID_16_ENCODE(BT_UUID_DIS_VAL)),
    BT_DATA(BT_DATA_UUID128_ALL, dfu_service_uuid.val, sizeof(dfu_service_uuid.val)),
};

//
// State and Characteristics
//

static void audio_ccc_config_changed_handler(const struct bt_gatt_attr *attr, uint16_t value)
{
    if (value == BT_GATT_CCC_NOTIFY) {
        LOG_INF("Client subscribed for notifications");
    } else if (value == 0) {
        LOG_INF("Client unsubscribed from notifications");
    } else {
        LOG_INF("Invalid CCC value: %u", value);
    }
}

static ssize_t audio_data_read_characteristic(struct bt_conn *conn,
                                              const struct bt_gatt_attr *attr,
                                              void *buf,
                                              uint16_t len,
                                              uint16_t offset)
{
    LOG_DBG("audio_data_read_characteristic");
    return bt_gatt_attr_read(conn, attr, buf, len, offset, NULL, 0);
}

static ssize_t audio_codec_read_characteristic(struct bt_conn *conn,
                                               const struct bt_gatt_attr *attr,
                                               void *buf,
                                               uint16_t len,
                                               uint16_t offset)
{
    uint8_t value[1] = {CODEC_ID};
    LOG_DBG("audio_codec_read_characteristic %d", CODEC_ID);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(value));
}

static ssize_t audio_data_write_handler(struct bt_conn *conn,
                                        const struct bt_gatt_attr *attr,
                                        const void *buf,
                                        uint16_t len,
                                        uint16_t offset,
                                        uint8_t flags)
{
    uint16_t amount = 400;
    int16_t *int16_buf = (int16_t *) buf;
    uint8_t *data = (uint8_t *) buf;
    bt_gatt_notify(conn, attr, &amount, sizeof(amount));
    amount = speak(len, buf);
    return len;
}

//
// DFU Service Handlers
//

static void dfu_ccc_config_changed_handler(const struct bt_gatt_attr *attr, uint16_t value)
{
    if (value == BT_GATT_CCC_NOTIFY) {
        LOG_INF("Client subscribed for notifications");
    } else if (value == 0) {
        LOG_INF("Client unsubscribed from notifications");
    } else {
        LOG_INF("Invalid CCC value: %u", value);
    }
}

static ssize_t dfu_control_point_write_handler(struct bt_conn *conn,
                                               const struct bt_gatt_attr *attr,
                                               const void *buf,
                                               uint16_t len,
                                               uint16_t offset,
                                               uint8_t flags)
{
    LOG_INF("dfu_control_point_write_handler");
    if (len == 1 && ((uint8_t *) buf)[0] == 0x06) {
        NRF_POWER->GPREGRET = 0xA8;
        NVIC_SystemReset();
    } else if (len == 2 && ((uint8_t *) buf)[0] == 0x01) {
        uint8_t notification_value = 0x10;
        bt_gatt_notify(conn, attr, &notification_value, sizeof(notification_value));

        NRF_POWER->GPREGRET = 0xA8;
        NVIC_SystemReset();
    }
    return len;
}

//
// Battery Service Handlers
//

#define BATTERY_REFRESH_INTERVAL 15000 // 15 seconds

void broadcast_battery_level(struct k_work *work_item);

K_WORK_DELAYABLE_DEFINE(battery_work, broadcast_battery_level);

void broadcast_battery_level(struct k_work *work_item)
{
    uint16_t battery_millivolt;
    uint8_t battery_percentage;
    if (battery_get_millivolt(&battery_millivolt) == 0 &&
        battery_get_percentage(&battery_percentage, battery_millivolt) == 0) {

        LOG_PRINTK("Battery at %d mV (capacity %d%%)\n", battery_millivolt, battery_percentage);

        // Use the Zephyr BAS function to set (and notify) the battery level
        int err = bt_bas_set_battery_level(battery_percentage);
        if (err) {
            LOG_ERR("Error updating battery level: %d", err);
        }
    } else {
        LOG_ERR("Failed to read battery level");
    }

    k_work_reschedule(&battery_work, K_MSEC(BATTERY_REFRESH_INTERVAL));
}

//
// Connection Callbacks
//

static void _transport_connected(struct bt_conn *conn, uint8_t err)
{
    struct bt_conn_info info = {0};

    err = bt_conn_get_info(conn, &info);
    if (err) {
        LOG_ERR("Failed to get connection info (err %d)", err);
        bt_conn_unref(conn);
        return;
    }

    LOG_INF("bluetooth activated");

    connection_count++;

    if (current_connection != NULL) {
        bt_conn_unref(current_connection);
    }
    current_connection = bt_conn_ref(conn);
    current_mtu = info.le.data_len->tx_max_len;
    LOG_INF("Transport connected");
    LOG_DBG("Interval: %d, latency: %d, timeout: %d", info.le.interval, info.le.latency, info.le.timeout);
    LOG_DBG("TX PHY %s, RX PHY %s", phy2str(info.le.phy->tx_phy), phy2str(info.le.phy->rx_phy));
    LOG_DBG("LE data len updated: TX (len: %d time: %d) RX (len: %d time: %d)",
            info.le.data_len->tx_max_len,
            info.le.data_len->tx_max_time,
            info.le.data_len->rx_max_len,
            info.le.data_len->rx_max_time);

    k_work_schedule(&battery_work, K_MSEC(100)); // run immediately

    is_connected = true;
}

static void _transport_disconnected(struct bt_conn *conn, uint8_t err)
{
    is_connected = false;

    LOG_INF("Transport disconnected");

    if (current_connection != NULL) {
        bt_conn_unref(current_connection);
        current_connection = NULL;
    }
    current_mtu = 0;
}

static bool _le_param_req(struct bt_conn *conn, struct bt_le_conn_param *param)
{
    LOG_INF("Transport connection parameters update request received.");
    LOG_DBG("Minimum interval: %d, Maximum interval: %d", param->interval_min, param->interval_max);
    LOG_DBG("Latency: %d, Timeout: %d", param->latency, param->timeout);

    return true;
}

static void _le_param_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency, uint16_t timeout)
{
    LOG_INF("Connection parameters updated.");
    LOG_DBG("[ interval: %d, latency: %d, timeout: %d ]", interval, latency, timeout);
}

static void _le_phy_updated(struct bt_conn *conn, struct bt_conn_le_phy_info *param)
{
    // LOG_DBG("LE PHY updated: TX PHY %s, RX PHY %s",
    //        phy2str(param->tx_phy), phy2str(param->rx_phy));
}

static void _le_data_length_updated(struct bt_conn *conn, struct bt_conn_le_data_len_info *info)
{
    LOG_DBG("LE data len updated: TX (len: %d time: %d)"
            " RX (len: %d time: %d)",
            info->tx_max_len,
            info->tx_max_time,
            info->rx_max_len,
            info->rx_max_time);
    current_mtu = info->tx_max_len;
}

// The single most useful signal when pairing misbehaves: it fires whether the procedure succeeded
// or failed, and carries the reason code the stack would otherwise keep to itself. Recorded into
// the pairing-status characteristic because serial is not readable on this board.
//
// The callback only exists in `struct bt_conn_cb` when SMP is compiled in, so both the function and
// the assignment below must be guarded or the default non-secure image stops compiling.
#if defined(CONFIG_BT_SMP)
static void _security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
    last_security_err = (uint8_t) err;
    last_security_level = (uint8_t) level;
    if (err) {
        LOG_ERR("security change failed: level %u, err %u", level, err);
    } else {
        LOG_INF("security changed: level %u", level);
    }
}
#endif

static struct bt_conn_cb _callback_references = {
    .connected = _transport_connected,
    .disconnected = _transport_disconnected,
#if defined(CONFIG_BT_SMP)
    .security_changed = _security_changed,
#endif
    .le_param_req = _le_param_req,
    .le_param_updated = _le_param_updated,
    .le_phy_updated = _le_phy_updated,
    .le_data_len_updated = _le_data_length_updated,
};

//
// Ring Buffer
//

#define NET_BUFFER_HEADER_SIZE 3
#define RING_BUFFER_HEADER_SIZE 2
static uint8_t tx_queue[NETWORK_RING_BUF_SIZE * (CODEC_OUTPUT_MAX_BYTES + RING_BUFFER_HEADER_SIZE)];
static uint8_t tx_buffer[CODEC_OUTPUT_MAX_BYTES + RING_BUFFER_HEADER_SIZE];
static uint8_t tx_buffer_2[CODEC_OUTPUT_MAX_BYTES + RING_BUFFER_HEADER_SIZE];
static uint32_t tx_buffer_size = 0;
static struct ring_buf ring_buf;

static bool write_to_tx_queue(uint8_t *data, size_t size)
{
    if (size > CODEC_OUTPUT_MAX_BYTES) {
        return false;
    }

    // Copy data (TODO: Avoid this copy)
    tx_buffer_2[0] = size & 0xFF;
    tx_buffer_2[1] = (size >> 8) & 0xFF;
    memcpy(tx_buffer_2 + RING_BUFFER_HEADER_SIZE, data, size);

    // Write to ring buffer
    int written =
        ring_buf_put(&ring_buf,
                     tx_buffer_2,
                     (CODEC_OUTPUT_MAX_BYTES + RING_BUFFER_HEADER_SIZE)); // It always fits completely or not at all
    if (written != CODEC_OUTPUT_MAX_BYTES + RING_BUFFER_HEADER_SIZE) {
        return false;
    } else {
        return true;
    }
}

static bool read_from_tx_queue()
{

    // Read from ring buffer
    // memset(tx_buffer, 0, sizeof(tx_buffer));
    tx_buffer_size =
        ring_buf_get(&ring_buf,
                     tx_buffer,
                     (CODEC_OUTPUT_MAX_BYTES + RING_BUFFER_HEADER_SIZE)); // It always fits completely or not at all
    if (tx_buffer_size != (CODEC_OUTPUT_MAX_BYTES + RING_BUFFER_HEADER_SIZE)) {
        // An empty queue is the normal idle state for the pusher, not an error.
        LOG_DBG("tx queue empty (%d bytes)", tx_buffer_size);
        return false;
    }

    // Adjust size
    tx_buffer_size = tx_buffer[0] + (tx_buffer[1] << 8);
    // LOG_PRINTK("tx_buffer_size %d\n",tx_buffer_size);

    return true;
}

//
// Pusher
//

// Thread
K_THREAD_STACK_DEFINE(pusher_stack, 4096);
static struct k_thread pusher_thread;
static uint16_t packet_next_index = 0;
static uint8_t pusher_temp_data[CODEC_OUTPUT_MAX_BYTES + NET_BUFFER_HEADER_SIZE];

// Sends the frame currently held in tx_buffer. The caller owns the dequeue so the same frame
// can also be handed to the recorder; this must not pull from the ring buffer itself.
static bool push_to_gatt(struct bt_conn *conn)
{
    // Push each frame
    uint8_t *buffer = tx_buffer + RING_BUFFER_HEADER_SIZE;
    uint32_t offset = 0;
    uint8_t index = 0;
    int retry_count = 0;
    const int max_retries = 3;

    while (offset < tx_buffer_size) {
        // Recombine packet
        uint32_t id = packet_next_index++;
        uint32_t packet_size = MIN(current_mtu - NET_BUFFER_HEADER_SIZE, tx_buffer_size - offset);
        pusher_temp_data[0] = id & 0xFF;
        pusher_temp_data[1] = (id >> 8) & 0xFF;
        pusher_temp_data[2] = index;
        memcpy(pusher_temp_data + NET_BUFFER_HEADER_SIZE, buffer + offset, packet_size);

        offset += packet_size;
        index++;

        retry_count = 0;
        while (retry_count < max_retries) {
            // Try send notification
            int err =
                bt_gatt_notify(conn, &audio_service.attrs[1], pusher_temp_data, packet_size + NET_BUFFER_HEADER_SIZE);

            // Log failure
            if (err) {
                LOG_DBG("bt_gatt_notify failed (err %d)", err);
                LOG_DBG("MTU: %d, packet_size: %d", current_mtu, packet_size + NET_BUFFER_HEADER_SIZE);
                k_sleep(K_MSEC(1));
                retry_count++;
                continue;
            }

            // Try to send more data if possible
            if (err == -EAGAIN || err == -ENOMEM) {
                retry_count++;
                continue;
            }

            // Break if success
            break;
        }

        if (retry_count >= max_retries) {
            LOG_ERR("Failed to send packet after %d retries", max_retries);
            return false;
        }
    }

    return true;
}
#define OPUS_PREFIX_LENGTH 1
#define OPUS_PADDED_LENGTH 80
#define MAX_WRITE_SIZE 440
static uint8_t storage_temp_data[MAX_WRITE_SIZE];
static uint32_t offset = 0;
static uint16_t buffer_offset = 0;
// Packs the frame currently held in tx_buffer into the 440-byte block and flushes the block to
// the card when full. Like push_to_gatt(), the caller owns the dequeue.
bool write_to_storage(void)
{ // max possible packing
    uint8_t *buffer = tx_buffer + 2;
    uint8_t packet_size = (uint8_t) (tx_buffer_size + OPUS_PREFIX_LENGTH);

    // buffer_offset = buffer_offset+amount_to_fill;
    // check if adding the new packet will cause a overflow
    if (buffer_offset + packet_size > MAX_WRITE_SIZE - 1) {

        storage_temp_data[buffer_offset] = tx_buffer_size;
        uint8_t *write_ptr = storage_temp_data;
        write_to_file(write_ptr, MAX_WRITE_SIZE);

        buffer_offset = packet_size;
        storage_temp_data[0] = tx_buffer_size;
        memcpy(storage_temp_data + 1, buffer, tx_buffer_size);

    } else if (buffer_offset + packet_size == MAX_WRITE_SIZE - 1) { // exact frame needed
        storage_temp_data[buffer_offset] = tx_buffer_size;
        memcpy(storage_temp_data + buffer_offset + 1, buffer, tx_buffer_size);
        buffer_offset = 0;
        uint8_t *write_ptr = (uint8_t *) storage_temp_data;
        write_to_file(write_ptr, MAX_WRITE_SIZE);

    } else {
        storage_temp_data[buffer_offset] = tx_buffer_size;
        memcpy(storage_temp_data + buffer_offset + 1, buffer, tx_buffer_size);
        buffer_offset = buffer_offset + packet_size;
    }

    return true;
}

static uint8_t heartbeat_count = 0;
void update_file_size()
{
    file_num_array[0] = storage_get_size();
    file_num_array[1] = get_offset();
}

// Roughly one second of audio at 100 frames/s.
#define FILE_SIZE_REFRESH_FRAMES 100

void pusher(void)
{
    k_msleep(500);
    static bool connection_was_true = false;

    while (1) {
        struct bt_conn *conn = current_connection;

        if (conn && !connection_was_true) {
            connection_was_true = true;
            // Refresh the advertised sizes so a client reading them right after connecting
            // sees the current recording rather than a stale value.
            update_file_size();
        } else if (!conn) {
            connection_was_true = false;
        }

        if (conn) {
            conn = bt_conn_ref(conn);
        }

        bool stream = false;
        if (conn && current_mtu >= MINIMAL_PACKET_SIZE) {
            stream = bt_gatt_is_subscribed(conn, &audio_service.attrs[1], BT_GATT_CCC_NOTIFY);
        }

        // One dequeue per iteration, fanned out to both sinks. The card records continuously
        // whether or not a phone is attached, and a subscribed phone still gets live audio.
        if (read_from_tx_queue()) {
            if (is_sd_on()) {
                k_mutex_lock(&write_sdcard_mutex, K_FOREVER);
                write_to_storage();
                k_mutex_unlock(&write_sdcard_mutex);
            }

            if (stream) {
                push_to_gatt(conn);
            }

            if (++heartbeat_count >= FILE_SIZE_REFRESH_FRAMES) {
                heartbeat_count = 0;
                file_num_array[0] = storage_get_size();
            }
        } else {
            // Queue drained. Yielding alone spun this thread flat out, which starved the
            // logger and buried the system in "not enough data" errors.
            k_sleep(K_MSEC(2));
        }

        if (conn) {
            bt_conn_unref(conn);
        }
    }
}
extern struct bt_gatt_service storage_service;
//
// Public functions
//
int bt_off()
{
    // First disconnect any active connections
    if (current_connection != NULL) {
        bt_conn_disconnect(current_connection, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        bt_conn_unref(current_connection);
        current_connection = NULL;
    }

    // Stop advertising
    int err = bt_le_adv_stop();
    if (err) {
        LOG_ERR("Failed to stop Bluetooth advertising %d", err);
    }

    // Disable Bluetooth
    err = bt_disable();
    if (err) {
        LOG_ERR("Failed to disable Bluetooth %d", err);
    }

    // Turn off other peripherals
    k_mutex_lock(&write_sdcard_mutex, K_FOREVER);
    sd_off();
    k_mutex_unlock(&write_sdcard_mutex);
    mic_off();

    // Ensure all Bluetooth resources are cleaned up
    is_connected = false;
    current_mtu = 0;

    return 0;
}
int bt_on()
{
    int err = bt_enable(NULL);
    bt_le_adv_start(BT_LE_ADV_CONN, bt_ad, ARRAY_SIZE(bt_ad), bt_sd, ARRAY_SIZE(bt_sd));
    bt_gatt_service_register(&storage_service);
    sd_on();
    mic_on();

    return 0;
}

// periodic advertising
int transport_start()
{
    k_mutex_init(&write_sdcard_mutex);

    // Configure callbacks
    bt_conn_cb_register(&_callback_references);

#if defined(CONFIG_BT_SMP)
    // Without this the stack knows exactly why a pairing attempt failed and discards it, which is
    // what made the first attempt at this feature pure guesswork.
    bt_conn_auth_info_cb_register(&auth_info_callbacks);
#endif

    // Enable Bluetooth
    int err = bt_enable(NULL);
    if (err) {
        LOG_ERR("Transport bluetooth init failed (err %d)", err);
        return err;
    }
    LOG_INF("Transport bluetooth initialized");

    // Bonds are useless unless they are reloaded here: without this the single bond slot looks
    // empty on every boot, the paired phone is asked to pair again, and the first stranger to
    // connect can claim the slot instead.
    //
    // Never fatal. Returning an error here would abort transport_start(), and main() treats that
    // as "stop", so the device would never advertise -- a corrupted bond area would turn into a
    // permanently silent board with no way in over the air to fix it. Losing the bonds and asking
    // the owner to pair again is always the better failure.
    if (IS_ENABLED(CONFIG_BT_SETTINGS)) {
        int serr = settings_load();
        if (serr) {
            LOG_ERR("Failed to load bonds (err %d); continuing unpaired", serr);
        }
    }

#ifdef CONFIG_OMI_ERASE_BONDS_ON_BOOT
    // Recovery image only; see the Kconfig help. Reflashing does not clear the bond partition and
    // there is no button, so this is the only way back from a bond you no longer control.
    err = bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY);
    LOG_WRN("erased all Bluetooth bonds (err %d) -- flash the normal image back now", err);
#endif

    //  Enable button
#ifdef CONFIG_OMI_ENABLE_BUTTON
    register_button_service();
#endif

#ifdef CONFIG_OMI_ENABLE_SPEAKER
    register_speaker_service();
#endif

    // Start advertising
    memset(storage_temp_data, 0, OPUS_PADDED_LENGTH * 4);
    bt_gatt_service_register(&storage_service);
    bt_gatt_service_register(&audio_service);
    bt_gatt_service_register(&time_sync_service);
    bt_gatt_service_register(&dfu_service);
    bt_gatt_service_register(&pairing_service);
    bt_gatt_service_register(&diagnostics_service);
    err = bt_le_adv_start(BT_LE_ADV_CONN, bt_ad, ARRAY_SIZE(bt_ad), bt_sd, ARRAY_SIZE(bt_sd));
    if (err) {
        LOG_ERR("Transport advertising failed to start (err %d)", err);
        return err;
    } else {
        LOG_INF("Advertising successfully started");
    }

    int battErr = battery_init();
    if (battErr) {
        LOG_ERR("Battery init failed (err %d)", battErr);
    } else {
        LOG_INF("Battery initialized");
    }

    // Start pusher
    ring_buf_init(&ring_buf, sizeof(tx_queue), tx_queue);
    k_thread_create(&pusher_thread,
                    pusher_stack,
                    K_THREAD_STACK_SIZEOF(pusher_stack),
                    (k_thread_entry_t) pusher,
                    NULL,
                    NULL,
                    NULL,
                    K_PRIO_PREEMPT(7),
                    0,
                    K_NO_WAIT);

    return 0;
}

struct bt_conn *get_current_connection()
{
    return current_connection;
}

int broadcast_audio_packets(uint8_t *buffer, size_t size)
{
    int retry_count = 0;
    const int max_retries = 3;

    while (retry_count < max_retries && !write_to_tx_queue(buffer, size)) {
        k_sleep(K_MSEC(1));
        retry_count++;
    }

    if (retry_count >= max_retries) {
        LOG_ERR("Failed to write to tx queue after %d retries", max_retries);
        return -1;
    }

    return 0;
}

void accel_off()
{
    gpio_pin_set_dt(&accel_gpio_pin, 0);
}

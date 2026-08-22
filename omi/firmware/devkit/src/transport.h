#ifndef TRANSPORT_H
#define TRANSPORT_H

#include <zephyr/bluetooth/gatt.h>
#include <zephyr/drivers/sensor.h>

/*
 * Permissions for every characteristic that exposes audio, recordings, the clock or DFU.
 *
 * These must track CONFIG_BT_SMP, which secure-pairing.conf turns on. Hardcoding the _ENCRYPT
 * variants makes those attributes permanently unreachable in a build without SMP: encryption can
 * never be established, so the peer gets "Insufficient Encryption" on every access forever, and
 * the device looks bricked while advertising perfectly normally. Reflashing does not fix it, which
 * sends you hunting a phantom GATT-cache bug on the host.
 */
#ifdef CONFIG_BT_SMP
#define OMI_PERM_READ BT_GATT_PERM_READ_ENCRYPT
#define OMI_PERM_WRITE BT_GATT_PERM_WRITE_ENCRYPT
#else
#define OMI_PERM_READ BT_GATT_PERM_READ
#define OMI_PERM_WRITE BT_GATT_PERM_WRITE
#endif

typedef struct sensors {

    struct sensor_value a_x;
    struct sensor_value a_y;
    struct sensor_value a_z;
    struct sensor_value g_x;
    struct sensor_value g_y;
    struct sensor_value g_z;
};
/**
 * @brief Initialize the BLE transport logic
 *
 * Initializes the BLE Logic
 *
 * @return 0 if successful, negative errno code if error
 */
int transport_start();

/**
 * @brief Release the Bluetooth bond. Call only after the recordings have been erased.
 *
 * Invoked by the storage thread at the end of storage_request_unbond_wipe(). Not a public entry
 * point for releasing the bond -- going through storage keeps the wipe-then-release order that
 * stops a new owner inheriting the previous owner's audio.
 */
void transport_finish_unbond(void);
int broadcast_audio_packets(uint8_t *buffer, size_t size);
struct bt_conn *get_current_connection();
int bt_on();
int bt_off();

void accel_off();
#endif
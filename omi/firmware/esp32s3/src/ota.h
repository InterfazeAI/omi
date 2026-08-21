#ifndef OTA_H
#define OTA_H

#include <Arduino.h>
#include <BLECharacteristic.h>

// Callback object for the OTA control characteristic. Owned by this module so
// there is exactly one definition of it; app.cpp used to declare a second class
// of the same name at file scope, which linked only by luck.
BLECharacteristicCallbacks *ota_create_control_callback();

// Set BLE characteristics (called after BLE init)
void ota_set_characteristics(BLECharacteristic *controlChar, BLECharacteristic *dataChar);

// Handle incoming OTA command
void ota_handle_command(uint8_t *data, size_t length);

// Process OTA in main loop (non-blocking)
void ota_loop();

// Get current OTA status
uint8_t ota_get_status();

// Check if OTA is in progress
bool ota_is_busy();

// Cancel any ongoing OTA operation
void ota_cancel();

// Notify status change via BLE
void ota_notify_status(uint8_t status, uint8_t progress = 0);

#endif // OTA_H

#ifndef BLE_SERVICE_H_
#define BLE_SERVICE_H_

#include <stdint.h> // uint32_t

/** @brief ENV_NODE Service UUID. */
#define BT_UUID_ENV_NODE_SERVICE_VAL \
    BT_UUID_128_ENCODE(0x9e844024, 0x1935, 0x465a, 0x94c7, 0xc82cbd35ecc1)

/** @brief TEMPERATURE Characteristic UUID. */
#define BT_UUID_ENV_NODE_TEMPERATURE_VAL \
    BT_UUID_128_ENCODE(0x9e844025, 0x1935, 0x465a, 0x94c7, 0xc82cbd35ecc1)

/** @brief INTERVAL Characteristic UUID. */
#define BT_UUID_ENV_NODE_INTERVAL_VAL \
    BT_UUID_128_ENCODE(0x9e844026, 0x1935, 0x465a, 0x94c7, 0xc82cbd35ecc1)

#define BT_UUID_ENV_NODE_SERVICE BT_UUID_DECLARE_128(BT_UUID_ENV_NODE_SERVICE_VAL)
#define BT_UUID_ENV_NODE_TEMPERATURE BT_UUID_DECLARE_128(BT_UUID_ENV_NODE_TEMPERATURE_VAL)
#define BT_UUID_ENV_NODE_INTERVAL BT_UUID_DECLARE_128(BT_UUID_ENV_NODE_INTERVAL_VAL)

/**
 * @brief Initialize the Bluetooth low energy connection.
 *  initialize BLE stack, start advertising
 * @return 0 on success or negative error code on failure.
 */
int BLEInit(void);

/**
 * @brief Send temperature notification to connected device.
 * @param temperature The temperature value to send in Celsius multiplied by 100 (e.g. 25.123C would be 25123).
 * note: the input temperature is a fixed-point representation to avoid using floating-point arithmetic.
 * need to divide by 100 to get the actual temperature in Celsius.
 */
int BLENotify(int32_t temperature);

#endif // BLE_SERVICE_H_
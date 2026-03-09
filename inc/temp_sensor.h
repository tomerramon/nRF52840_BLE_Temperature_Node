#ifndef TEMP_SENSOR_H_
#define TEMP_SENSOR_H_

#include <stdint.h> // int32_t

/**
 * @brief Initialize the temperature sensor.
 * @return 0 on success or negative error code on failure.
 */
int TempSensorInit(void);

/**
 * @brief Read the internal temperature sensor on the board.
 * @param[out] out_temp The output temperature in Celsius multiplied by 100(e.g. 25.12 C is represented as 2512).
 *Divide by 100 to get the actual temperature in Celsius.
 *Note: min/max tracking is updated on every successful read.
 *TempSensorGetMin/Max() return INT32_MAX/INT32_MIN until
 *the first successful read.
 * @return 0 on success or negative error code on failure.
 */
int TempSensorRead(int32_t *out_temp);

/**
 * @brief Get the minimum temperature recorded since boot.
 * @return Minimum temperature in Celsius multiplied by 100.
 *         Returns INT32_MAX if no successful read has occurred yet.
 */
int32_t TempSensorGetMin(void);

/**
 * @brief Get the maximum temperature recorded since boot.
 * @return Maximum temperature in Celsius multiplied by 100.
 *         Returns INT32_MIN if no successful read has occurred yet.
 */
int32_t TempSensorGetMax(void);

#endif /* TEMP_SENSOR_H_ */

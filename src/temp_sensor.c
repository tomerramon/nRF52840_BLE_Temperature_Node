#include <zephyr/kernel.h>         // k_sleep
#include <zephyr/drivers/sensor.h> // device_is_ready, sensor_sample_fetch
#include <zephyr/logging/log.h>    // LOG_MODULE_REGISTER

#include "temp_sensor.h"

LOG_MODULE_REGISTER(temp_sensor, LOG_LEVEL_DBG);

#define FILTER_WINDOW_SIZE 10

static int32_t min_temp = INT32_MAX;
static int32_t max_temp = INT32_MIN;

static const struct device *temp_dev;

static int32_t ApplyMovingAverage(int32_t new_sample);

int TempSensorInit(void)
{
    temp_dev = DEVICE_DT_GET(DT_NODELABEL(temp));
    if (!device_is_ready(temp_dev))
    {
        LOG_ERR("Temperature sensor device not ready");
        return -ENODEV;
    }

    LOG_INF("Temperature sensor initialized successfully");
    return 0;
}

int TempSensorRead(int32_t *out_temp)
{
    int ret;
    struct sensor_value sensor_val;

    ret = sensor_sample_fetch(temp_dev);
    if (0 > ret)
    {
        LOG_ERR("Failed to fetch temperature sensor sample: %d", ret);
        return ret;
    }

    ret = sensor_channel_get(temp_dev, SENSOR_CHAN_DIE_TEMP, &sensor_val);
    if (0 > ret)
    {
        LOG_ERR("Failed to get temperature sensor channel value: %d", ret);
        return ret;
    }

    *out_temp = ApplyMovingAverage(sensor_val.val1 * 100 + (sensor_val.val2 / 10000));

    LOG_DBG("Read temperature: %d.%02d C", sensor_val.val1, sensor_val.val2 / 10000);

    min_temp = (*out_temp < min_temp) ? *out_temp : min_temp;
    max_temp = (*out_temp > max_temp) ? *out_temp : max_temp;

    return 0;
}

int32_t TempSensorGetMin(void)
{
    return min_temp;
}

int32_t TempSensorGetMax(void)
{
    return max_temp;
}

static int32_t ApplyMovingAverage(int32_t new_sample)
{
    static int32_t filter_buffer[FILTER_WINDOW_SIZE];
    static uint8_t filter_head = 0;
    static uint8_t filter_count = 0;
    int64_t sum = 0;

    filter_buffer[filter_head] = new_sample;
    filter_head = (filter_head + 1) % FILTER_WINDOW_SIZE;

    if (filter_count < FILTER_WINDOW_SIZE)
    {
        filter_count++;
    }

    for (uint8_t i = 0; i < filter_count; i++)
    {
        sum += filter_buffer[i];
    }

    return (int32_t)(sum / filter_count);
}

#include <zephyr/kernel.h>           // k_sem_take
#include <zephyr/logging/log.h>      // LOG_MODULE_REGISTER
#include <zephyr/drivers/watchdog.h> // wdt_install_timeout, wdt_feed

#include "temp_sensor.h" // TempSensorInit, TempSensorRead
#include "app_timer.h"   // TimerInit, TimerGetSemaphore
#include "ble_service.h" // BLEInit, BLENotify

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

#define WDT_TIMEOUT_MS (MAX_INTERVAL_MS * 2)

static const struct device *wdt = DEVICE_DT_GET(DT_NODELABEL(wdt0));

int main(void)
{
    int ret;
    int32_t temperature;
    int wdt_channel;

    static const struct wdt_timeout_cfg wdt_cfg = {
        .window.min = 0,
        .window.max = WDT_TIMEOUT_MS, // 20 seconds in ms
        .callback = NULL,             // NULL = reset immediately on timeout
        .flags = WDT_FLAG_RESET_SOC,  // Reset the entire SoC on timeout
    };

    wdt_channel = wdt_install_timeout(wdt, &wdt_cfg);
    if (0 > wdt_channel)
    {
        LOG_ERR("Failed to install watchdog timeout: %d", wdt_channel);
        return wdt_channel;
    }

    ret = wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG);
    if (0 > ret)
    {
        LOG_ERR("Failed to setup watchdog: %d", ret);
        return ret;
    }

    ret = TempSensorInit();
    if (0 > ret)
    {
        LOG_ERR("Failed to initialize temperature sensor: %d", ret);
        return ret;
    }

    TimerInit();

    ret = BLEInit();

    if (ret)
    {
        LOG_ERR("Failed to initialize BLE stack: %d", ret);
        return ret;
    }

    while (true)
    {
        k_sem_take(TimerGetSemaphore(), K_FOREVER);
        ret = TempSensorRead(&temperature);
        if (0 > ret)
        {
            LOG_ERR("Failed to read temperature sensor: %d", ret);
        }
        else
        {
            LOG_DBG("Current temperature: %d.%02d C", temperature / 100, temperature % 100);
            LOG_DBG("Min: %d.%02d C, Max: %d.%02d C",
                    TempSensorGetMin() / 100, TempSensorGetMin() % 100,
                    TempSensorGetMax() / 100, TempSensorGetMax() % 100);
            ret = BLENotify(temperature);
            if (ret && ret != -EACCES)
            {
                LOG_WRN("Failed to notify: %d", ret);
            }
        }

        wdt_feed(wdt, wdt_channel);
        LOG_DBG("Watchdog fed successfully");
    }
    return 0;
}

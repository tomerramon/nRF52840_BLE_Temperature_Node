#include <zephyr/logging/log.h> //LOG_MODULE_REGISTER

#include "app_timer.h"

static void timer_expiry_handler(struct k_timer *timer);

LOG_MODULE_REGISTER(app_timer, LOG_LEVEL_DBG);

K_SEM_DEFINE(timer_sem, 0, 1);
K_TIMER_DEFINE(app_timer, timer_expiry_handler, NULL);

static uint32_t timer_interval_ms = DEFAULT_INTERVAL_MS; // Default interval of 1 second

void TimerInit(void)
{
    TimerSetInterval(timer_interval_ms);
    LOG_INF("Application timer initialized successfully");
}

void TimerSetInterval(uint32_t interval_ms)
{
    LOG_INF("Application timer interval set to %u ms", interval_ms);
    if (interval_ms < MIN_INTERVAL_MS || interval_ms > MAX_INTERVAL_MS)
    {
        LOG_ERR("Rejected interval %u ms (allowed: %u-%u ms), Interval remain as is.", interval_ms, MIN_INTERVAL_MS, MAX_INTERVAL_MS);
        return;
    }
    timer_interval_ms = interval_ms;
    k_timer_start(&app_timer, K_MSEC(interval_ms), K_MSEC(interval_ms));
}

struct k_sem *TimerGetSemaphore(void)
{
    return &timer_sem;
}

uint32_t TimerGetInterval(void)
{
    return timer_interval_ms;
}

/* helper function that run every time the timer expires */
static void timer_expiry_handler(struct k_timer *timer)
{
    k_sem_give(&timer_sem);
}
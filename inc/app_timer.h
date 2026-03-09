#ifndef APP_TIMER_H_
#define APP_TIMER_H_

#include <zephyr/kernel.h>
#include <stdint.h> // uint32_t

#define MAX_INTERVAL_MS 10000
#define MIN_INTERVAL_MS 200
#define DEFAULT_INTERVAL_MS 1000

/**
 * @brief Initialize the application timer with the default interval.
 *        Starts the periodic timer immediately.
 */
void TimerInit(void);

/**
 * @brief Set (or update) the application timer interval.
 *        Restarts the timer from zero with the new period.
 * @param interval_ms The timer interval in milliseconds.
 *                    Must be in range [MIN_INTERVAL_MS, MAX_INTERVAL_MS];
 *                    the caller is responsible for validation.
 */
void TimerSetInterval(uint32_t interval_ms);

/**
 * @brief Get the semaphore that is signaled on every timer expiry.
 *        Use k_sem_take() on this to block until the next sample tick.
 * @return Pointer to the timer semaphore.
 */
struct k_sem *TimerGetSemaphore(void);

/**
 * @brief Get the current timer interval.
 * @return The timer interval in milliseconds.
 */
uint32_t TimerGetInterval(void);

#endif // APP_TIMER_H_
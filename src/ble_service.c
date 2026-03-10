#include <zephyr/bluetooth/bluetooth.h> // bt_enable
#include <zephyr/bluetooth/conn.h>      // BT_LE_ADV_CONN
#include <zephyr/bluetooth/gatt.h>      // bt_gatt_notify
#include <zephyr/logging/log.h>         // LOG_MODULE_REGISTER
#include <zephyr/sys/atomic.h>          // atomic_t, atomic_set, atomic_get
#include <zephyr/sys/util.h>            // memcpy
#include <dk_buttons_and_leds.h>        // dk_set_led_on
#include <sys/types.h>                  // ssize_t

#include "ble_service.h" // BLEInit, BLENotify
#include "app_timer.h"   // TimerGetInterval

LOG_MODULE_REGISTER(ble_controller, LOG_LEVEL_DBG);

#define BASE_DEVICE_NAME CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(BASE_DEVICE_NAME) - 1)

/* Dynamic suffix format: '_' + 4 hex chars + null terminator = 6 extra bytes */
#define DEVICE_NAME_SUFFIX_LEN 6
#define DEVICE_NAME_BUF_LEN (DEVICE_NAME_LEN + DEVICE_NAME_SUFFIX_LEN)
#define CON_STATUS_LED DK_LED2

/*
 * Index of the Temperature characteristic *value* attribute within env_node_svc.attrs[].
 * Layout: [0]=primary service, [1]=char declaration, [2]=char value, [3]=CCC,
 *         [4]=interval char declaration, [5]=interval char value.
 * If attributes are reordered this MUST be updated accordingly.
 */
#define TEMP_CHRC_ATTR_IDX 2

/*
 * Preferred BLE connection parameters requested from the central after connecting.
 *
 * Rationale:
 *   - interval_min = 100 ms, interval_max = 500 ms (units of 1.25 ms → 80 / 400):
 *       The device samples at 200 ms–10 s. A 100–500 ms connection interval means
 *       notifications are delivered within at most one interval of their sample tick,
 *       while still saving significant radio-on time vs the default 7.5 ms interval.
 *   - latency = 0: The peripheral must stay responsive every event to receive
 *       interval WRITE commands from the central without delay.
 *   - timeout = 4000 ms (400 × 10 ms):
 *       Must satisfy: timeout > interval_max × (latency + 1) × 2
 *       → 4000 > 500 × 1 × 2 = 1000 ms  ✓
 *       Gives enough headroom for a phone screen-off radio duty-cycle.
 *
 * Note: These are *preferred* parameters. The central (phone) is not obligated
 * to accept them. The negotiated values are logged in on_le_param_updated().
 */
#define CONN_INTERVAL_MIN_UNITS BT_GAP_MS_TO_CONN_INTERVAL(100) /* 80  × 1.25 ms = 100 ms */
#define CONN_INTERVAL_MAX_UNITS BT_GAP_MS_TO_CONN_INTERVAL(500) /* 400 × 1.25 ms = 500 ms */
#define CONN_LATENCY 0
#define CONN_TIMEOUT_UNITS 400 /* 400 × 10 ms = 4000 ms */

static const struct bt_le_conn_param preferred_conn_params =
    BT_LE_CONN_PARAM_INIT(CONN_INTERVAL_MIN_UNITS,
                          CONN_INTERVAL_MAX_UNITS,
                          CONN_LATENCY,
                          CONN_TIMEOUT_UNITS);

/*
 * These flags are written from BT stack callbacks (BT thread context)
 * and read from the main application thread. atomic_t provides lock-free,
 * race-free access without needing a mutex.
 */
static atomic_t is_connected = ATOMIC_INIT(0);
static atomic_t is_notifications_enabled = ATOMIC_INIT(0);
static atomic_t current_temperature = ATOMIC_INIT(0);

static struct k_work adv_work;

/*
 * device_name is populated by SetDynamicDeviceName() before advertising starts.
 */
static char device_name[DEVICE_NAME_BUF_LEN];

static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
};

static struct bt_data sd[] = {
    BT_DATA(BT_DATA_NAME_COMPLETE, device_name, DEVICE_NAME_BUF_LEN),
};

static void adv_work_handler(struct k_work *work)
{
    int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));

    if (err)
    {
        LOG_ERR("Advertising failed to start (err %d)", err);
        return;
    }

    LOG_INF("Advertising successfully started");
}

static void advertising_start(void)
{
    k_work_submit(&adv_work);
}

static void recycled_cb(void)
{
    LOG_INF("Connection object available from previous conn. Disconnect is complete!");
    advertising_start();
}

static void on_connected(struct bt_conn *conn, uint8_t err)
{
    if (err)
    {
        LOG_ERR("Connection failed (err %u)", err);
        return;
    }

    LOG_INF("Connected");
    atomic_set(&is_connected, 1);
    dk_set_led_on(CON_STATUS_LED);

    /*
     * Request preferred connection parameters from the central.
     * This is advisory — the central (phone) may accept, modify, or reject.
     * The outcome is logged in on_le_param_updated() below.
     */
    err = bt_conn_le_param_update(conn, &preferred_conn_params);
    if (err)
    {
        LOG_WRN("Connection parameter update request failed (err %d)", err);
    }
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
    LOG_INF("Disconnected (reason %u)", reason);
    atomic_set(&is_connected, 0);
    atomic_set(&is_notifications_enabled, 0);
    dk_set_led_off(CON_STATUS_LED);
}

/*
 * Called when the central proposes new connection parameters.
 * Returning true accepts them unconditionally.
 */
static bool on_le_param_req(struct bt_conn *conn, struct bt_le_conn_param *param)
{
    if (param->interval_min < CONN_INTERVAL_MIN_UNITS || param->interval_max > CONN_INTERVAL_MAX_UNITS)
    {
        LOG_WRN("Central requests invalid conn params: interval [%u, %u] latency %u timeout %u",
                param->interval_min, param->interval_max,
                param->latency, param->timeout);
        return false; /* reject */
    }

    LOG_DBG("Central requests conn params: interval [%u, %u] latency %u timeout %u",
            param->interval_min, param->interval_max,
            param->latency, param->timeout);
    return true; /* accept */
}

/*
 * Called after connection parameters have been successfully updated.
 * Logs the actual negotiated values so you can verify in RTT/UART logs.
 */
static void on_le_param_updated(struct bt_conn *conn, uint16_t interval,
                                uint16_t latency, uint16_t timeout)
{
    /* interval is in units of 1.25 ms */
    LOG_INF("Conn params updated: interval %u (%.2f ms), latency %u, timeout %u ms",
            interval, (double)interval * 1.25, latency, timeout * 10);
}

BT_CONN_CB_DEFINE(connection_callbacks) = {
    .connected = on_connected,
    .disconnected = on_disconnected,
    .recycled = recycled_cb,
    .le_param_req = on_le_param_req,         /* central requests param change   */
    .le_param_updated = on_le_param_updated, /* negotiation completed, log result */
};

static ssize_t ReadTemperature(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf, uint16_t len, uint16_t offset)
{
    int32_t temp = (int32_t)atomic_get(&current_temperature);

    return bt_gatt_attr_read(conn, attr, buf, len, offset, &temp, sizeof(temp));
}

static void OnCccChanged(const struct bt_gatt_attr *attr, uint16_t value)
{
    atomic_set(&is_notifications_enabled, (value == BT_GATT_CCC_NOTIFY) ? 1 : 0);
    LOG_INF("Temperature notifications %s", atomic_get(&is_notifications_enabled) ? "enabled" : "disabled");
}

static ssize_t ReadInterval(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf, uint16_t len, uint16_t offset)
{
    uint32_t interval = TimerGetInterval();

    return bt_gatt_attr_read(conn, attr, buf, len, offset, &interval, sizeof(interval));
}

static ssize_t WriteInterval(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    uint32_t new_interval;

    if (len != sizeof(uint32_t))
    {
        LOG_ERR("Invalid interval length: %u (expected %zu)", len, sizeof(uint32_t));
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    memcpy(&new_interval, buf, sizeof(new_interval));

    if (new_interval < MIN_INTERVAL_MS || new_interval > MAX_INTERVAL_MS)
    {
        LOG_WRN("Rejected interval %u ms (allowed: %u-%u ms)", new_interval,
                MIN_INTERVAL_MS, MAX_INTERVAL_MS);
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }

    TimerSetInterval(new_interval);
    return len;
}

BT_GATT_SERVICE_DEFINE(env_node_svc,
                       BT_GATT_PRIMARY_SERVICE(BT_UUID_ENV_NODE_SERVICE),

                       BT_GATT_CHARACTERISTIC(BT_UUID_ENV_NODE_TEMPERATURE,
                                              BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                                              BT_GATT_PERM_READ,
                                              ReadTemperature, NULL, NULL),
                       BT_GATT_CCC(OnCccChanged, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

                       BT_GATT_CHARACTERISTIC(BT_UUID_ENV_NODE_INTERVAL,
                                              BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
                                              BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
                                              ReadInterval, WriteInterval, NULL), );

static void SetDynamicDeviceName(void)
{
    int err;
    bt_addr_le_t addr[1];
    size_t count = 1;

    bt_id_get(addr, &count);

    if (count == 0)
    {
        LOG_ERR("Failed to get BT address, keeping default name: %s", BASE_DEVICE_NAME);
        return;
    }

    uint8_t last_byte = addr[0].a.val[0];
    uint8_t prev_byte = addr[0].a.val[1];

    snprintf(device_name, sizeof(device_name), "%s_%02X%02X", BASE_DEVICE_NAME, prev_byte, last_byte);

    /* Update scan data length to match the actual dynamic name length */
    sd[0].data_len = strlen(device_name);

    err = bt_set_name(device_name);
    if (err)
    {
        LOG_ERR("Failed to set device name (err %d)", err);
    }
    else
    {
        LOG_INF("Device name set to: %s", device_name);
    }
}

int BLEInit(void)
{
    int err;

    err = dk_leds_init();
    if (err)
    {
        LOG_ERR("LEDs init failed (err %d)", err);
        return err;
    }

    err = bt_enable(NULL);
    if (err)
    {
        LOG_ERR("Bluetooth init failed (err %d)", err);
        return err;
    }

    SetDynamicDeviceName();

    k_work_init(&adv_work, adv_work_handler);
    advertising_start();

    return 0;
}

int BLENotify(int32_t temperature)
{
    if (!atomic_get(&is_connected))
    {
        LOG_DBG("Cannot send notification: no device connected");
        return -EACCES;
    }

    if (!atomic_get(&is_notifications_enabled))
    {
        LOG_DBG("Cannot send notification: notifications disabled by client");
        return -EACCES;
    }

    atomic_set(&current_temperature, temperature);

    return bt_gatt_notify(NULL, &env_node_svc.attrs[TEMP_CHRC_ATTR_IDX],
                          &temperature, sizeof(temperature));
}
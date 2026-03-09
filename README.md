# BLE Temperature Node — nRF52840

A BLE peripheral firmware built with **nRF Connect SDK (Zephyr RTOS)** that periodically reads the nRF52840 internal temperature sensor and exposes it over a custom GATT service. Designed for low power: the CPU sleeps between samples, waking only on timer expiry or BLE events.

> **SDK Choice:** This project uses **nRF Connect SDK v3.2.3 (Zephyr RTOS)** — not the legacy nRF5 SDK (SoftDevice).  
> All BLE APIs (`bt_enable`, `bt_gatt_notify`, `BT_GATT_SERVICE_DEFINE`) are from Zephyr's built-in BLE stack. The build system is Zephyr's `west` + CMake. There is no SoftDevice HEX, no `sd_ble_*` calls, and no nRF5 SDK Makefile structure anywhere in this project.

---

## Table of Contents

- [SDK & Toolchain](#sdk--toolchain)
- [Project Structure](#project-structure)
- [How to Build & Flash](#how-to-build--flash)
- [How to Test](#how-to-test-with-nrf-connect-app)
- [Architecture Overview](#architecture-overview)
- [BLE GATT Service](#ble-gatt-service)
- [Low-Power Strategy](#low-power-strategy)
- [Watchdog Strategy](#watchdog-strategy)
- [Bonus Features](#bonus-features)
- [Current Consumption Estimate](#current-consumption-estimate)

---

## SDK & Toolchain

| Component         | Version               |
|-------------------|-----------------------|
| nRF Connect SDK   | v3.2.3                |
| Zephyr RTOS       | bundled with SDK      |
| CMake             | ≥ 3.20.0              |
| Target Board      | `nrf52840dk/nrf52840` |

---

## Project Structure

```
ble_temp_node/
├── src/
│   ├── main.c           # Entry point: init, main loop, WDT feed
│   ├── ble_service.c    # BLE stack init, GATT service, advertising
│   ├── temp_sensor.c    # Sensor read, moving average, min/max tracking
│   └── app_timer.c      # Periodic timer, semaphore signal
├── inc/
│   ├── ble_service.h
│   ├── temp_sensor.h
│   └── app_timer.h
├── CMakeLists.txt
├── prj.conf
└── README.md
```

---

## How to Build & Flash

### Prerequisites

The recommended setup uses the **nRF Connect extension pack for VS Code** — it manages the SDK, toolchain, and board targets from inside the editor with no manual PATH configuration.

1. Install [Visual Studio Code](https://code.visualstudio.com/)
2. Open VS Code → Extensions (`Ctrl+Shift+X`) → search **"nRF Connect for VS Code Extension Pack"** → Install
3. The extension pack will prompt you to install the **nRF Connect SDK**. Click **Install SDK**, select version **v3.2.3**, and let it download the toolchain automatically.
4. Once installed, open the **nRF Connect** sidebar panel (Nordic icon on the left) — your SDK and toolchain paths will be shown and ready.

### Build & Flash

All building and flashing is done through the **nRF Connect for VS Code** extension — no terminal commands needed.

1. Open the project folder in VS Code (`File → Open Folder`)
2. In the **nRF Connect** sidebar panel, click **+ Add Build Configuration**
3. Select board: `nrf52840dk/nrf52840` → click **Build Configuration**
4. Once the build completes, click **Flash** in the same panel — it will program the DK over the J-Link USB connection automatically

### Monitor Serial Logs

In VS Code, open the **Serial Terminal** panel (bottom bar) and connect at **115200 baud** to the DK's COM port. Alternatively, use the **RTT Viewer** from the nRF Connect panel for zero-latency log output over the debug probe.

---

## How to Test with nRF Connect App

1. Install **nRF Connect for Mobile** (iOS or Android).
2. Power on the DK. **LED2 is OFF** (not connected).
3. Open nRF Connect → **Scanner** tab → look for a device named `ENV_NODE_XXYY` where `XXYY` are the last 2 bytes of the BLE address.
4. Tap **Connect**.  **LED2 turns ON** confirming connection.
5. Navigate to the **ENV_NODE custom service** (UUID starts with `9e844024-...`).

### Read Temperature
- Tap the **Temperature characteristic** (UUID `9e844025-...`) → **Read**.
- Value is a signed `int32_t` in units of `°C × 100` (little-endian).
- Example: `0xBC 0x09 0x00 0x00` = `0x000009BC` = 2492 = **24.92 °C**

### Enable Notifications
- Tap the **↓** (notify) button on the Temperature characteristic.
- Notifications arrive at the current sampling interval (default: **1000 ms**).

### Change Sampling Interval
- Tap the **Interval characteristic** (UUID `9e844026-...`) → **Write**.
- Write a 4-byte little-endian `uint32_t` value in milliseconds.
- Valid range: **200 ms – 10,000 ms**. Values outside this range are rejected with ATT error `Value Not Allowed`.
- Example — set to 2 seconds: write `0xD0 0x07 0x00 0x00` (2000 = `0x000007D0`).

### Disconnect
- Tap **Disconnect**. LED2 turns OFF. The device automatically restarts advertising.

---

## Architecture Overview

### Threads

Zephyr is a multi-threaded RTOS. This firmware uses three threads — two created by the system, one by the application:

| Thread         | Created by     | Priority | Role                                                                 |
|----------------|----------------|----------|----------------------------------------------------------------------|
| **main**       | Zephyr kernel  | 0        | Init sequence, sensor read loop, WDT feed. Sleeps on semaphore between ticks. |
| **BT RX/TX**   | BLE stack      | varies   | Handles all BLE radio events: connections, disconnections, GATT reads/writes, notifications. Calls `on_connected`, `on_disconnected`, `ReadTemperature`, `WriteInterval`. |
| **System work queue** | Zephyr kernel | -1 | Executes `adv_work_handler` to start advertising. Used to safely call BT API from a non-ISR context after disconnect. |

> **Why the work queue for advertising?**  
> `bt_le_adv_start()` cannot be called directly from a BT callback — it would deadlock the BT stack. Submitting it as a `k_work` item defers it to the system work queue thread, which runs outside the BT context.

> **Why `atomic_t` for shared flags?**  
> `is_connected`, `is_notifications_enabled`, and `current_temperature` are written by the BT thread and read by the main thread. `atomic_t` guarantees safe concurrent access without a mutex.

---

### UML — Component & Data Flow

```
┌─────────────────────────────────────────────────────────────────────┐
│  HARDWARE / PERIPHERALS                                             │
│                                                                     │
│   ┌──────────┐    RTC1 interrupt     ┌────────────────────────┐    │
│   │  RTC1    │──────────────────────►│  k_timer (app_timer.c) │    │
│   └──────────┘                       └──────────┬─────────────┘    │
│                                                  │ k_sem_give       │
│   ┌──────────┐    radio interrupt               │                  │
│   │  RADIO   │──────────────────────►  BT stack thread             │
│   └──────────┘                                  │                  │
└─────────────────────────────────────────────────│──────────────────┘
                                                  │
┌─────────────────────────────────────────────────▼──────────────────┐
│  APPLICATION THREADS                                                │
│                                                                     │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │  main thread                                                 │  │
│  │                                                              │  │
│  │  [boot]                                                      │  │
│  │   WDT setup ──► TempSensorInit ──► TimerInit ──► BLEInit    │  │
│  │                                                              │  │
│  │  [loop — CPU sleeps here until semaphore fires]              │  │
│  │   k_sem_take(K_FOREVER)                                      │  │
│  │         │                                                    │  │
│  │         ▼                                                    │  │
│  │   TempSensorRead()  ──► ApplyMovingAverage() ──► min/max    │  │
│  │         │                                                    │  │
│  │         ▼                                                    │  │
│  │   BLENotify(temp)   ──► bt_gatt_notify() ──► BT stack       │  │
│  │         │                                                    │  │
│  │         ▼                                                    │  │
│  │   wdt_feed()                                                 │  │
│  └──────────────────────────────────────────────────────────────┘  │
│                                                                     │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │  BT stack thread  (managed by Zephyr BLE stack)              │  │
│  │                                                              │  │
│  │   on_connected()    ──► atomic_set(is_connected)             │  │
│  │   on_disconnected() ──► atomic_set(is_connected = 0)         │  │
│  │                         k_work_submit(adv_work)              │  │
│  │   WriteInterval()   ──► TimerSetInterval()                   │  │
│  │   ReadTemperature() ──► atomic_get(current_temperature)      │  │
│  └──────────────────────────────────────────────────────────────┘  │
│                                                                     │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │  System work queue thread                                    │  │
│  │                                                              │  │
│  │   adv_work_handler() ──► bt_le_adv_start()                   │  │
│  └──────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────┘
```

### Module Responsibilities

| Module           | Responsibility                                                              |
|------------------|-----------------------------------------------------------------------------|
| `main.c`         | Orchestration: init sequence, main loop, WDT feed                          |
| `ble_service.c`  | BLE stack init, dynamic device name, GATT service definition, advertising   |
| `temp_sensor.c`  | Sensor read, 10-sample moving average filter, min/max tracking              |
| `app_timer.c`    | Periodic `k_timer` → signals `k_sem` to unblock the main loop              |

---

## BLE GATT Service

### Service UUID
`9e844024-1935-465a-94c7-c82cbd35ecc1`

### Characteristics

| Characteristic | UUID        | Properties      | Format                        | Notes                              |
|----------------|-------------|-----------------|-------------------------------|------------------------------------|
| Temperature    | `9e844025-…`| Read, Notify    | `int32_t`, LE, `°C × 100`    | Filtered value; notify on each tick|
| Interval       | `9e844026-…`| Read, Write     | `uint32_t`, LE, milliseconds  | Range: 200–10000 ms                |

### Device Name

Format: `ENV_NODE_XXYY`  
`XX` and `YY` are bytes 1 and 0 of the public BLE address (last 2 bytes), printed as hex. This gives each device a unique, stable, human-readable suffix derived from hardware — no manual provisioning needed.

---

## Low-Power Strategy

The firmware is fully **event-driven** — there are no busy-wait loops or `k_sleep(K_MSEC(x))` spin delays anywhere in the application.

### What Sleeps

When the main thread calls `k_sem_take(K_FOREVER)`, Zephyr's scheduler finds no other runnable thread and puts the CPU into **sleep mode** (ARM WFE/WFI). The nRF52840 enters its low-power idle state automatically.

### What Wakes the CPU

| Wake Source        | Mechanism                                        |
|--------------------|--------------------------------------------------|
| Sampling timer     | `k_timer` backed by the nRF52840 RTC1 peripheral. Fires an interrupt → `k_sem_give` → main thread unblocks |
| BLE events         | Zephyr BLE stack uses the RTC0 peripheral and radio interrupt. Connection, disconnection, and CCC writes are all interrupt-driven |
| Watchdog           | WDT interrupt (if callback set) or direct SoC reset |

### How to Estimate Current (No Hardware Required)

Use the **Nordic online Power Profiler** at [devzone.nordicsemi.com/power](https://devzone.nordicsemi.com/power) — no hardware, no account, runs in the browser.

Configure it as follows to match this firmware:

| Setting | Value |
|---|---|
| Protocol | Bluetooth LE |
| Role | Peripheral |
| Advertising interval | 100 ms (FAST_1 preset) |
| Connection interval | 7.5–15 ms (Zephyr default) |
| TX power | 0 dBm |
| Notification interval | matches your sampling interval |
| CPU sleep between events | enabled |

The tool will output estimated average current in µA across advertising, connected, and idle states. This is a reasonable estimate for the nRF52840 at 3V — real measurements will vary based on PCB layout and regulator efficiency, but the order of magnitude will be correct.

**Rough expected values for reference:**

| State | Estimated current |
|---|---|
| CPU sleeping, radio off | ~3–5 µA |
| Advertising (100 ms interval) | ~200–400 µA average |
| Connected + notifying (1s interval) | ~50–100 µA average |

---

## Watchdog Strategy

The WDT is configured with a timeout of `MAX_INTERVAL_MS × 2` = **20,000 ms**.

**Why 2× the maximum interval?**  
`wdt_feed()` is called at the end of every main loop iteration, which fires at the user-configured sampling interval (200–10,000 ms). Setting the WDT to exactly 2× `MAX_INTERVAL_MS` means:
- Under any valid sampling interval, the loop will always feed the WDT well before it expires.
- If the main thread genuinely hangs or deadlocks — stuck waiting on a semaphore that never comes, or blocked inside a driver call — the WDT fires after 20 seconds and resets the SoC.
- The 2× factor provides a safe margin against one missed feed (e.g. a single slow sensor read) without masking real hangs.

**`WDT_OPT_PAUSE_HALTED_BY_DBG`** — The WDT pauses when the debugger halts the CPU. This prevents spurious resets during development and JTAG stepping.

**`WDT_FLAG_RESET_SOC`** — On timeout, the entire SoC resets (not just the CPU core), ensuring all peripherals including the BLE radio return to a clean state.

---

## Bonus Features

### Moving Average Filter
A 10-sample circular buffer in `temp_sensor.c` smooths out ADC noise from the internal temperature sensor. The filter correctly handles the warm-up period — it divides by the actual number of samples collected, not the fixed window size.

### Min/Max Tracking
`TempSensorGetMin()` and `TempSensorGetMax()` return the extreme values recorded since boot. These are logged on every sample in `DEBUG` builds. Values are `INT32_MAX` / `INT32_MIN` until the first successful read.

### Dynamic Device Name
Unique per-device name derived from the BT address — requires zero configuration and survives firmware updates since the BT address is in UICR/factory data.

### Connection Status LED
LED2 on the DK mirrors the BLE connection state in real time.
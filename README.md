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
- [Design Decisions & Trade-offs](#design-decisions--trade-offs)
- [Watchdog Strategy](#watchdog-strategy)
- [Bonus Features](#bonus-features)

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
┌────────────────────────────────────────────────────────────────────┐
│  HARDWARE / PERIPHERALS                                            │
│                                                                    │
│   ┌──────────┐    RTC1 interrupt     ┌────────────────────────┐    │
│   │  RTC1    │──────────────────────►│  k_timer (app_timer.c) │    │
│   └──────────┘                       └──────────┬─────────────┘    │
│                                                 │ k_sem_give       │
│   ┌──────────┐    radio interrupt               │                  │
│   │  RADIO   │──────────────────────►  BT stack thread             │
│   └──────────┘                                  │                  │
└─────────────────────────────────────────────────│──────────────────┘
                                                  │
┌─────────────────────────────────────────────────▼──────────────────┐
│  APPLICATION THREADS                                               │
│                                                                    │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │  main thread                                                 │  │
│  │                                                              │  │
│  │  [boot]                                                      │  │
│  │   WDT setup ──► TempSensorInit ──► TimerInit ──► BLEInit     │  │
│  │                                                              │  │
│  │  [loop — CPU sleeps here until semaphore fires]              │  │
│  │   k_sem_take(K_FOREVER)                                      │  │
│  │         │                                                    │  │
│  │         ▼                                                    │  │
│  │   TempSensorRead()  ──► ApplyMovingAverage() ──► min/max     │  │
│  │         │                                                    │  │
│  │         ▼                                                    │  │
│  │   BLENotify(temp)   ──► bt_gatt_notify() ──► BT stack        │  │
│  │         │                                                    │  │
│  │         ▼                                                    │  │
│  │   wdt_feed()                                                 │  │
│  └──────────────────────────────────────────────────────────────┘  │
│                                                                    │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │  BT stack thread  (managed by Zephyr BLE stack)              │  │
│  │                                                              │  │
│  │   on_connected()    ──► atomic_set(is_connected)             │  │
│  │                     ──► bt_conn_le_param_update()            │  │
│  │   on_disconnected() ──► atomic_set(is_connected = 0)         │  │
│  │   recycled_cb()     ──► k_work_submit(adv_work)              │  │
│  │                         (connection object freed; safe to    │  │
│  │                          restart advertising only now)       │  │
│  │   WriteInterval()   ──► TimerSetInterval()                   │  │
│  │   ReadTemperature() ──► atomic_get(current_temperature)      │  │
│  └──────────────────────────────────────────────────────────────┘  │
│                                                                    │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │  System work queue thread                                    │  │
│  │                                                              │  │
│  │   adv_work_handler() ──► bt_le_adv_start()                   │  │
│  └──────────────────────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────────────────────┘
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

### How to Measure Current Consumption

#### Option 1 — Nordic PPK2 (recommended, ~$100)
The **Power Profiler Kit II** is the purpose-built tool for this job. Connect it in **ampere meter mode** between the DK's `VDDMAIN` supply and the board's power input. Use the **nRF Connect Power Profiler** desktop app to record a live current trace. You get µA resolution, timestamps, and a visual breakdown of advertising bursts, connection events, and sleep periods. This is the gold-standard approach for nRF52840 power analysis.

#### Option 2 — Oscilloscope + shunt resistor (if you have a scope but no PPK2)
Place a small shunt resistor (e.g. **10 Ω**) in series with the VDD supply line. Probe the voltage across it with your oscilloscope. Current = V_shunt / R_shunt. This gives you real waveform visibility into burst events (advertising, TX, sensor read). Limitations: scope probes add ~10 pF capacitance which can disturb short bursts; not suitable for sub-µA sleep current without a high-gain differential probe.

#### Option 3 — Multimeter (limited, not recommended for dynamic loads)
A standard multimeter in current mode can measure average current in a steady state (e.g. advertising-only with no connection). It cannot capture burst behavior — the internal sampling rate is too slow and the burden voltage of the current range can disturb the supply. Useful only for sanity-checking order-of-magnitude in a stable state, not for understanding peak vs. average split.

#### Option 4 — Nordic Online Power Profiler (no hardware needed, estimation only)
Use [devzone.nordicsemi.com/power](https://devzone.nordicsemi.com/power) for a software estimate without any hardware. Configure it to match this firmware:

| Setting | Value |
|---|---|
| Protocol | Bluetooth LE |
| Role | Peripheral |
| Advertising interval | 100 ms (`BT_LE_ADV_CONN_FAST_1` preset) |
| Connection interval | 100–500 ms (peripheral preference set in `ble_service.c`) |
| TX power | 0 dBm |
| Notification interval | matches your sampling interval (default 1000 ms) |
| CPU sleep between events | enabled |

This gives a model-based estimate, not a real measurement. Use it to validate your design direction before bringing up hardware.

---

### Current Consumption Estimates

All figures are for nRF52840 at 3.0 V, DC/DC converter disabled (LDO mode, which is the nRF52840 DK default). Enabling DC/DC (`CONFIG_DCDC_NATIVE_BAAA=y`) typically reduces active-state current by ~30%.

| State | What is happening | Estimated avg current |
|---|---|---|
| **Deep sleep, no BLE** | CPU in WFI, all peripherals off, no advertising | ~3–5 µA |
| **Advertising only** | CPU sleeping, radio wakes every 100 ms for 3 ADV PDUs | ~300–500 µA avg |
| **Connected, idle** | CPU sleeping, radio polls at 100–500 ms connection interval, no notifications | ~20–60 µA avg |
| **Connected + notifying** | CPU wakes at sample interval, reads sensor (~0.5 ms), sends BLE notification (~2–5 ms TX burst), sleeps | ~50–120 µA avg at 1 s interval |
| **Peak TX burst** | Radio transmitting at 0 dBm | ~8–10 mA peak (lasts ~1–2 ms) |
| **Sensor read active** | CPU active, sensor fetch via Zephyr driver | ~1–3 mA peak (lasts ~0.5 ms) |

**Key observation:** the dominant power consumer at a 1-second sample interval is the BLE radio during advertising and connection events, not the CPU or sensor. Reducing the advertising interval or increasing the connection interval directly improves average current. At a 10-second sample interval, the device spends ~99.9% of the time sleeping — average current approaches the deep-sleep floor.

> **Important:** these are engineering estimates derived from Nordic datasheets and the nRF52840 Product Specification (PS v1.7, Table 32–34). Real measurements on your specific board will differ based on PCB layout, regulator efficiency, crystal startup behaviour, and SDK sleep configuration. Always validate with a PPK2 or equivalent before making battery life claims.

---

## Design Decisions & Trade-offs

Every non-trivial choice in this project has a reason. This section documents them explicitly.

---

### SDK Choice: nRF Connect SDK (Zephyr) vs. nRF5 SDK (SoftDevice)

| | nRF Connect SDK (Zephyr) | nRF5 SDK (SoftDevice) |
|---|---|---|
| BLE stack | Zephyr built-in (open source) | Nordic SoftDevice (closed binary) |
| Build system | `west` + CMake | Makefile + SEGGER Embedded Studio |
| Driver model | Devicetree + Zephyr drivers | Register-level + nRF5 SDK HAL |
| RTOS | Zephyr (fully integrated) | Bare-metal or FreeRTOS add-on |
| Future support | Active, Nordic's strategic direction | Maintenance mode only |

**Decision: nRF Connect SDK.**

Reasons: NCS is Nordic's current and future platform. nRF5 SDK is in maintenance mode — new silicon (nRF54 series) is NCS-only. Zephyr's built-in BLE stack, device tree, and `west` build system are all actively developed and well documented. The SoftDevice approach requires managing a closed binary HEX blob alongside application code, which adds complexity with no benefit for a project of this scope.

Trade-off accepted: NCS has a steeper initial learning curve and longer build times than nRF5 SDK. For a production project this is a one-time cost.

---

### Event-Driven Design: Semaphore vs. `k_sleep` vs. Busy-Wait

**Decision: `k_sem_take(K_FOREVER)` in the main loop.**

Three alternatives were considered:

- **Busy-wait (`while (!time_expired) {}`)** — burns CPU cycles continuously, prevents any sleep. Completely unacceptable for a battery-conscious design.
- **`k_sleep(K_MSEC(interval))`** — works, but couples the sleep duration to the main loop rather than the timer. If the sensor read or BLE notify takes variable time, drift accumulates. Also makes dynamic interval changes awkward (you'd need to abort and restart the sleep).
- **`k_sem_take(K_FOREVER)` + `k_timer`** — the timer ISR is the single source of truth for timing. It fires at exactly the configured interval regardless of how long the main loop body takes. Dynamic interval changes (`TimerSetInterval`) take effect immediately on the next timer restart. The main thread is fully suspended between ticks, letting the CPU sleep.

Trade-off: slightly more code (a separate `app_timer.c` module) in exchange for correct periodic timing and clean dynamic interval support.

---

### Error Handling Strategy

**Decision: log the error, propagate the negative errno up to the caller, let `main()` decide.**

The alternatives were:
- **`__ASSERT` / `k_panic()`** — immediately fatal. Appropriate for truly unrecoverable conditions (e.g. corrupted kernel state), but too aggressive for peripheral init failures on a DK where you want to see the log before any reset.
- **Silent ignore** — dangerous. A failed `bt_enable()` that isn't propagated means the device runs silently with no BLE. The root cause would never be found.
- **Log + propagate** — each module logs the specific failure with context (`LOG_ERR`) and returns the raw errno. `main()` decides the recovery strategy: for critical failures (WDT, sensor, BLE), it returns from `main()`, which triggers a Zephyr kernel panic and visible log output. For non-critical failures (a single missed notify), it logs a warning and continues.

The pattern used throughout: **negative return value = error, 0 = success**, consistent with Zephyr's own API convention.

---

### Temperature Representation: Fixed-Point `int32_t × 100` vs. `float`

**Decision: `int32_t` in units of °C × 100 (e.g. 24.92 °C = 2492).**

The nRF52840 Cortex-M4F has a hardware FPU, so `float` would work. But floating-point is avoided for three reasons:

1. **BLE transmission** — `float` is 4 bytes with IEEE 754 encoding. Sending it over BLE requires the client to know the encoding. `int32_t` is simpler: the client just divides by 100. The BT SIG `IEEE_11073_SFLOAT` type exists for this, but adds complexity with no benefit here.
2. **Comparison and filtering** — integer arithmetic in `ApplyMovingAverage()` and min/max tracking is exact. Float comparisons can have rounding surprises.
3. **Precision** — the nRF52840 internal sensor has ±0.25 °C accuracy. Two decimal places of °C is more than sufficient; float's 7 significant digits would be meaningless precision.

---

### Moving Average Window: 10 Samples

**Decision: 10-sample circular buffer.**

The nRF52840 internal temperature sensor has inherent noise of ±0.25 °C (datasheet spec) and is also affected by self-heating from the SoC. A moving average reduces random noise without introducing a long lag on genuine temperature changes. 10 samples at the default 1-second interval means the filter has a 10-second memory — enough to smooth noise, short enough to track real environmental changes. A larger window (e.g. 30) would smooth more but lag more. A smaller window (e.g. 3) barely helps.

Trade-off: the filter tracks **filtered** min/max, not raw min/max. This means the logged min/max values may not reflect the true hardware peak — they reflect the smoothed signal. This is a deliberate choice: reporting a filtered extreme is more useful than reporting a single noisy spike.

---

### Custom 128-bit UUIDs vs. BT SIG Standard UUIDs

**Decision: custom 128-bit UUIDs.**

The BT SIG defines standard UUIDs for `Temperature` (0x2A6E) and similar characteristics. Using them would make the service recognizable by generic BLE tools. However, the standard Temperature characteristic format uses `sint16` in units of 0.01 °C — different from this project's `int32_t × 100` format.

Using a custom UUID is the honest choice: the format is non-standard, so advertising it under a standard UUID would be misleading. A custom UUID makes it explicit that a custom client or setup step is required. The UUIDs used (`9e844024–...`) are random 128-bit values with no collision risk.

---

### Re-advertising After Disconnect: `recycled_cb` vs. `on_disconnected`

**Decision: start advertising from `recycled_cb`, not `on_disconnected`.**

This is a subtle but important Zephyr BLE correctness point. When `on_disconnected` fires, the connection object is still alive — the BT stack has not finished cleaning up. Calling `bt_le_adv_start()` at that point can fail or corrupt BT stack state.

`recycled_cb` fires only after the connection object has been fully freed and returned to the pool. It is the correct and safe point to restart advertising. This is explicitly documented in Zephyr's connection management API.

Additionally, `bt_le_adv_start()` is submitted via `k_work` (deferred to the system work queue) rather than called directly from `recycled_cb`, because BT API calls from within BT callbacks can cause re-entrancy issues. The work queue runs in a separate thread context, outside the BT stack's own execution context.

---



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

### Connection Interval Preference
After connecting, the peripheral requests a **100–500 ms** connection interval via `bt_conn_le_param_update()`. Rationale: the fastest sampling rate is 200 ms, so a ≤500 ms interval ensures notifications are delivered within one connection event of their sample tick, while saving significant radio-on time versus the 7.5 ms Zephyr default. Slave latency is set to 0 so the peripheral stays responsive to incoming interval WRITE commands. The actual negotiated values are logged by `on_le_param_updated()` — check your serial/RTT log to see what the phone accepted.

### Moving Average Filter
A 10-sample circular buffer in `temp_sensor.c` smooths out ADC noise from the internal temperature sensor. The filter correctly handles the warm-up period — it divides by the actual number of samples collected, not the fixed window size.

### Min/Max Tracking
`TempSensorGetMin()` and `TempSensorGetMax()` return the extreme values recorded since boot. These are logged on every sample in `DEBUG` builds. Values are `INT32_MAX` / `INT32_MIN` until the first successful read.

### Dynamic Device Name
Unique per-device name derived from the BT address — requires zero configuration and survives firmware updates since the BT address is in UICR/factory data.

### Connection Status LED
LED2 on the DK mirrors the BLE connection state in real time.

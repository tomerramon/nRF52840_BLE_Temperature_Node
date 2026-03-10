# BLE Temperature Node — nRF52840

A BLE peripheral firmware built with **nRF Connect SDK (Zephyr RTOS)** that periodically reads the nRF52840 internal temperature sensor and exposes it over a custom GATT service. Designed for low power: the CPU sleeps between samples, waking only on timer expiry or BLE events.

> **SDK Choice:** This project uses **nRF Connect SDK v3.2.3 (Zephyr RTOS)** — not the legacy nRF5 SDK (SoftDevice).  
> All BLE APIs (`bt_enable`, `bt_gatt_notify`, `BT_GATT_SERVICE_DEFINE`) are from Zephyr's built-in BLE stack.

---

## Table of Contents

- [Getting the Code](#getting-the-code)
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

## Getting the Code

```bash
git clone https://github.com//ble_temp_node.git
cd ble_temp_node
```

The repository is a self-contained NCS application — no separate SDK repository to clone. The only external dependency is the nRF Connect SDK itself, installed once via the VS Code extension (see [How to Build & Flash](#how-to-build--flash) below).

### First-Time Setup (one-time only)

1. Install VS Code and the **nRF Connect for VS Code Extension Pack**.
2. The extension will prompt you to install the **nRF Connect SDK v3.2.3** — do this once. It downloads the full Zephyr tree, toolchain (GCC ARM), and `west` tool automatically (~5–10 minutes, ~5 GB disk space).
3. Open the cloned `ble_temp_node` folder in VS Code. The nRF Connect panel will detect the `CMakeLists.txt` and `prj.conf` and recognise it as an NCS application automatically.

No `west init`, no `west update`, no manual PATH configuration needed.

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
├── CMakeLists.txt        # Tells west what to compile
├── prj.conf              # Kconfig: enables BLE, sensor, WDT, logging
└── README.md
```

---

## How to Build & Flash

### Prerequisites

1. Install [Visual Studio Code](https://code.visualstudio.com/)
2. Open VS Code → Extensions (`Ctrl+Shift+X`) → search **"nRF Connect for VS Code Extension Pack"** → Install
3. Click **Install SDK**, select **v3.2.3**, and let it download automatically.
4. Open the **nRF Connect** sidebar panel (Nordic icon) — SDK and toolchain paths will be shown and ready.

### Build & Flash

1. Open the project folder in VS Code (`File → Open Folder`)
2. In the nRF Connect panel → **+ Add Build Configuration**
3. Select board: `nrf52840dk/nrf52840` → **Build Configuration**
4. Once built, click **Flash** — programs the DK over J-Link USB automatically.

### Monitor Serial Logs

Open the **Serial Terminal** panel in VS Code, connect at **115200 baud**. Alternatively, use the **RTT Viewer** from the nRF Connect panel for zero-latency log output.

---

## How to Test with nRF Connect App

1. Install **nRF Connect for Mobile** (iOS or Android).
2. Power on the DK — **LED2 is OFF** (not connected).
3. Open nRF Connect → **Scanner** tab → find `ENV_NODE_XXYY` (last 2 bytes of BLE address).
4. Tap **Connect** — **LED2 turns ON**.
5. Navigate to the **ENV_NODE custom service** (UUID `9e844024-...`).

### Read Temperature
- Tap **Temperature characteristic** (UUID `9e844025-...`) → **Read**.
- Value is a signed `int32_t`, little-endian, units of `°C × 100`.
- Example: `0xBC 0x09 0x00 0x00` = 2492 = **24.92 °C**

### Enable Notifications
- Tap the **↓** (notify) button on the Temperature characteristic.
- Notifications arrive at the current sampling interval (default: **1000 ms**).

### Change Sampling Interval
- Tap **Interval characteristic** (UUID `9e844026-...`) → **Write**.
- Write a 4-byte little-endian `uint32_t` in milliseconds. Valid range: **200–10,000 ms**.
- Example — set to 2 seconds: `0xD0 0x07 0x00 0x00` (= 2000).
- Out-of-range values are rejected with ATT error `0x13` (Value Not Allowed).
- The board log confirms: `Application timer interval set to XXXX ms`.

### Disconnect
- Tap **Disconnect** — LED2 turns OFF. Device automatically restarts advertising.

---

## Architecture Overview

### Threads

Zephyr is a preemptive multi-threaded RTOS. **Lower numeric priority = higher urgency.** Priorities split into two domains:
- **Preemptive (priority ≥ 0):** can be interrupted by any higher-priority thread at any time.
- **Cooperative (priority < 0):** once running, yields voluntarily — never preempted mid-execution.

| Thread | Created by | Priority | Type | Role |
|---|---|---|---|---|
| **main** | Zephyr kernel | 0 | Preemptive | Init sequence, sensor read loop, WDT feed. Blocks on semaphore between ticks — CPU sleeps. |
| **BT RX/TX** | Zephyr BLE stack | ~-7 (see below) | Cooperative | Drives all BLE radio events: connections, disconnections, GATT reads/writes, notifications. |
| **System work queue** | Zephyr kernel | -1 | Cooperative | Executes deferred `k_work` items. Used here to restart advertising safely after disconnect. |

#### About the BT thread priority

The BLE stack creates several internal threads (e.g. `bt_rx` for HCI processing). Their priorities are set by Kconfig symbols inside the stack — you don't set them manually. The default is typically `-7`, controlled by `CONFIG_BT_RX_PRIO`. You can override this in `prj.conf` but it's rarely needed.

**Why cooperative (negative priority)?** BLE has hard real-time timing constraints — connection events must be handled on a tight schedule. Cooperative threads run to completion without being preempted by lower-priority threads like `main` (priority 0), which is exactly what the BT stack needs.

**Impact on the application:** the BT thread can interrupt `main` at any time. This is why `is_connected`, `is_notifications_enabled`, and `current_temperature` are `atomic_t` — the BT thread can write them while `main` is reading them.

---

### The System Work Queue

The system work queue is a Zephyr cooperative thread (priority -1) that processes a FIFO of `k_work` items. When you call `k_work_submit(&my_work)`, the item is queued and the handler runs later from the work queue's own thread context.

**Why is this needed for advertising restart?**

`recycled_cb()` fires when the BT stack has fully freed the connection object — this is the correct and safe point to restart advertising (`on_disconnected` fires earlier, while cleanup is still in progress, and calling `bt_le_adv_start()` there can corrupt BT stack state).

But even inside `recycled_cb`, calling `bt_le_adv_start()` directly is unsafe — it's still running inside a BT callback, meaning the BT stack is on the call stack. Calling back into the BT API from there risks re-entrancy and deadlock.

The solution:

```
recycled_cb()                    ← runs inside BT thread
    └─ k_work_submit(&adv_work)  ← just queues an item, returns immediately

System work queue thread         ← runs outside BT context
    └─ adv_work_handler()
           └─ bt_le_adv_start()  ← safe here
```

> Using the system work queue instead of a dedicated thread avoids allocating a new stack (512–2048 bytes) for a function that runs once per reconnect. The work queue already exists and is idle between events.


---

### Runtime Data Flow

```
 ┌──────────────────────────────────────────────────────────────────────┐
 │                            main.c                                    │
 │                                                                      │
 │  + main()                                                            │
 │    - wdt_install_timeout() / wdt_setup() / wdt_feed()  [Zephyr WDT]  │
 │    - TempSensorInit()                                                │
 │    - TimerInit()                                                     │
 │    - BLEInit()                                                       │
 │    - k_sem_take(TimerGetSemaphore(), K_FOREVER)  [loop]              │
 │    - TempSensorRead(&temperature)                                    │
 │    - BLENotify(temperature)                                          │
 └───────────┬──────────────────────┬──────────────────┬────────────────┘
             │ calls                │ calls            │ calls
             ▼                      ▼                  ▼
 ┌───────────────────┐  ┌──────────────────────┐  ┌──────────────────────┐
 │   temp_sensor.c   │  │    app_timer.c       │  │   ble_service.c      │
 │                   │  │                      │  │                      │
 │ + TempSensorInit()│  │ + TimerInit()        │  │ + BLEInit()          │
 │ + TempSensorRead()│  │ + TimerSetInterval() │  │ + BLENotify()        │
 │ + TempSensorGetMin│  │ + TimerGetInterval() │  │                      │
 │ + TempSensorGetMax│  │ + TimerGetSemaphore()│  │ - SetDynamicName()   │
 │                   │  │                      │  │ - advertising_start()│
 │ - ApplyMovingAvg()│  │ - timer_expiry_      │  │ - on_connected()     │
 │ - filter_buffer[] │  │   handler()          │  │ - on_disconnected()  │
 │ - min_temp        │  │ - k_sem (internal)   │  │ - recycled_cb()      │
 │ - max_temp        │  │ - k_timer(internal)  │  │ - ReadTemperature()  │
 │                   │  │                      │  │ - WriteInterval()    │
 │ [Zephyr sensor    │  │ [Zephyr k_timer,     │  │ - OnCccChanged()     │
 │  driver API]      │  │  k_sem API]          │  │                      │
 └───────────────────┘  └──────────────────────┘  │ [Zephyr BT API,      │
                                                  │  dk_leds API]        │
                                                  │                      │
                                                  │ calls──┬─────────────┤
                                                  │        ▼             │
                                                  │  app_timer.c         │
                                                  │  TimerSetInterval()  │
                                                  │  TimerGetInterval()  │
                                                  └──────────────────────┘

 ┌───────────────────┐  ┌────────────────────┐  ┌─────────────────────┐
 │   temp_sensor.h   │  │    app_timer.h     │  │   ble_service.h     │
 │  (interface)      │  │  (interface)       │  │  (interface)        │
 │                   │  │                    │  │                     │
 │  TempSensorInit   │  │  TimerInit         │  │  BLEInit            │
 │  TempSensorRead   │  │  TimerSetInterval  │  │  BLENotify          │
 │  TempSensorGetMin │  │  TimerGetInterval  │  │  UUID defines       │
 │  TempSensorGetMax │  │  TimerGetSemaphore │  │                     │
 │                   │  │  MIN/MAX/DEFAULT   │  │                     │
 └───────────────────┘  └────────────────────┘  └─────────────────────┘

 Dependency summary:
   main.c         → temp_sensor.h, app_timer.h, ble_service.h
   ble_service.c  → app_timer.h  (reads/sets interval on BLE write)
   temp_sensor.c  → (no project dependencies, only Zephyr drivers)
   app_timer.c    → (no project dependencies, only Zephyr kernel)
```

---

### Runtime Data Flow Diagram

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
│  │  main thread  (priority 0)                                   │  │
│  │                                                              │  │
│  │  [boot]                                                      │  │
│  │   WDT setup ──► TempSensorInit ──► TimerInit ──► BLEInit     │  │
│  │                                                              │  │
│  │  [loop]  k_sem_take(K_FOREVER)  ← CPU sleeps here            │  │
│  │               │                                              │  │
│  │               ▼                                              │  │
│  │       TempSensorRead()  ──► ApplyMovingAverage() ──► min/max │  │
│  │               │                                              │  │
│  │               ▼                                              │  │
│  │       BLENotify(temp)   ──► bt_gatt_notify() ──► BT stack    │  │
│  │               │                                              │  │
│  │               ▼                                              │  │
│  │           wdt_feed()                                         │  │
│  └──────────────────────────────────────────────────────────────┘  │
│                                                                    │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │  BT stack thread  (~priority -7)                             │  │
│  │                                                              │  │
│  │  on_connected()    ──► atomic_set(is_connected)              │  │
│  │                    ──► bt_conn_le_param_update()             │  │
│  │  on_disconnected() ──► atomic_set(is_connected = 0)          │  │
│  │  recycled_cb()     ──► k_work_submit(adv_work)               │  │
│  │  WriteInterval()   ──► TimerSetInterval()                    │  │
│  │  ReadTemperature() ──► atomic_get(current_temperature)       │  │
│  └──────────────────────────────────────────────────────────────┘  │
│                                                                    │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │   System work queue thread (priority -1)                     │  │
│  │   adv_work_handler() ──► bt_le_adv_start()                   │  │
│  └──────────────────────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────────────────────┘
```

### Module Responsibilities

| Module | Responsibility |
|---|---|
| `main.c` | Orchestration: init sequence, main loop, WDT feed |
| `ble_service.c` | BLE stack init, dynamic device name, GATT service, advertising |
| `temp_sensor.c` | Sensor read, 10-sample moving average, min/max tracking |
| `app_timer.c` | Periodic `k_timer` → signals `k_sem` to unblock the main loop |

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

Format: `ENV_NODE_XXYY` — `XX` and `YY` are the last 2 bytes of the public BLE address in hex. Gives each device a unique, stable suffix derived from hardware with no manual provisioning needed.

---

## Low-Power Strategy

The firmware is fully **event-driven** — no busy-wait loops or `k_sleep()` spin delays anywhere.

### What Sleeps

When the main thread calls `k_sem_take(K_FOREVER)`, Zephyr's scheduler finds no other runnable thread and puts the CPU into sleep (ARM WFI). The nRF52840 enters its low-power idle state automatically between every sample.

### What Wakes the CPU

| Wake Source | Mechanism |
|---|---|
| Sampling timer | `k_timer` backed by RTC1. Fires interrupt → `k_sem_give` → main thread unblocks |
| BLE events | BLE stack uses RTC0 + radio interrupt. All connection and GATT events are interrupt-driven |
| Watchdog | WDT timeout triggers SoC reset |

### How to Measure Current Consumption

**Option 1 — Nordic PPK2 (recommended)**  
Connect the Power Profiler Kit II in ampere-meter mode between the DK's `VDDMAIN` and the board supply. Use the nRF Connect Power Profiler desktop app to record a live trace with µA resolution and timestamps.

**Option 2 — Nordic Online Power Profiler (no hardware needed)**  
Use [devzone.nordicsemi.com/power](https://devzone.nordicsemi.com/power) for a model-based estimate. Configure it as:

| Setting | Value |
|---|---|
| Protocol | Bluetooth LE, Peripheral |
| Advertising interval | 100 ms (`BT_LE_ADV_CONN_FAST_1`) |
| Connection interval | 100–500 ms |
| TX power | 0 dBm |
| Notification interval | 1000 ms (default) |
| CPU sleep between events | enabled |

This is an estimate, not a real measurement — but good enough to validate design direction.

---

### Current Consumption Estimates

Figures for nRF52840 at 3.0 V, LDO mode (DK default). DC/DC (`CONFIG_DCDC_NATIVE_BAAA=y`) reduces active current ~30%.

| State | What is happening | Duration / cycle | Current | Avg contribution |
|---|---|---|---|---|
| **Deep sleep** | CPU in WFI, radio off, RTC running | ~990 ms / 1000 ms | ~4 µA | ~3.9 µA |
| **Sensor read** | CPU wakes, fetches temp via Zephyr driver | ~0.5 ms | ~2 mA peak | ~1.0 µA |
| **BLE TX burst** | Radio transmits notification at 0 dBm | ~1.5 ms | ~9 mA peak | ~13.5 µA |
| **BLE conn event (idle)** | Radio polls at connection interval, no data | ~1 ms per 200 ms | ~6 mA peak | ~30 µA |
| **Advertising** (unconnected) | 3 ADV PDUs every 100 ms | ~1.5 ms / 100 ms | ~9 mA peak | ~135 µA |

### How to Calculate Average Current

Use the duty-cycle method — for each state, multiply its current by the fraction of time in that state, then sum:

```
I_avg = Σ ( I_state × T_state ) / T_total
```

**Example: connected + notifying at 1 s interval, 200 ms connection interval**.

| State | Current | Duration | Contribution |
|---|---|---|---|
| Sleep (WFI) | 4 µA | ~992 ms | 4 × (992/1000) = **3.97 µA** |
| Sensor read | 2 mA | ~0.5 ms | 2000 × (0.5/1000) = **1.00 µA** |
| BLE TX (notification) | 9 mA | ~1.5 ms | 9000 × (1.5/1000) = **13.50 µA** |
| BLE conn events (×5, no data) | 6 mA | ~1 ms × 5 | 6000 × (5/1000) = **30.00 µA** |
| **Total I_avg** | | **1000 ms** | **≈ 48.5 µA** |

At 3.0 V, this corresponds to **~145 µW** average power.

For a **2000 mAh coin cell (CR2032 = 225 mAh, Li-AA = 2000 mAh)**:

```
Battery life (hours) = Battery capacity (mAh) / I_avg (mA)
                     = 2000 mAh / 0.0485 mA
                     ≈ 41,000 hours ≈ 4.7 years   (ideal, no self-discharge)

For a CR2032 (225 mAh):
                     = 225 / 0.0485 ≈ 4,640 hours ≈ 193 days
```

**Key observation:** the dominant power consumer at a 1-second sample interval is the BLE radio during advertising and connection events, not the CPU or sensor. Reducing the advertising interval or increasing the connection interval directly improves average current. At a 10-second sample interval, the device spends ~99% of the time sleeping — average current approaches the deep-sleep floor.

> **Important:**
> - these are engineering estimates derived from Nordic datasheets and the nRF52840 Product Specification (PS v1.7, Table 32–34). Not real measurements from my specific board, so there might be deferenses based on different hardware and software configurations.
> - The nRF52840 DK powers via USB or a 3.3 V regulator — these estimates apply to a standalone battery-powered design, not the DK itself.
>  Always validate with a PPK2 or equivalent before making battery life claims.

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

**Decision: nRF Connect SDK.**

Reasons: NCS is Nordic's current and future platform. nRF5 SDK is in maintenance mode. Zephyr's built-in BLE stack, device tree, and `west` build system are all actively developed and well documented.

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

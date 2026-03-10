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
| Sleep | 4 µA | 992 ms | **3.97 µA** |
| Sensor read | 2 mA | 0.5 ms | **1.00 µA** |
| BLE TX (notification) | 9 mA | 1.5 ms | **13.50 µA** |
| BLE conn events (×5) | 6 mA | 1 ms × 5 | **30.00 µA** |
| **I_avg** | | **1000 ms** | **≈ 48.5 µA** |


> These are estimates based on Nordic datasheets (nRF52840 PS v1.7). Real values will differ based on board layout, regulator, and SDK config. The DK itself draws more due to onboard circuitry — these figures apply to a standalone battery-powered design. Always validate with a PPK2 before making battery life claims.

**Key observation:** the dominant power consumer at a 1-second sample interval is the BLE radio during advertising and connection events, not the CPU or sensor. Reducing the advertising interval or increasing the connection interval directly improves average current. At a 10-second sample interval, the device spends ~99% of the time sleeping — average current approaches the deep-sleep floor.

## Design Decisions & Trade-offs

### SDK Choice: NCS (Zephyr) vs. nRF5 SDK (SoftDevice)

| | nRF Connect SDK | nRF5 SDK |
|---|---|---|
| BLE stack | Zephyr built-in (open source) | SoftDevice (closed binary) |
| Build system | `west` + CMake | Makefile + SEGGER Embedded Studio |
| Driver model | Devicetree + Zephyr drivers | Register-level HAL |
| Status | Active, Nordic's strategic direction | Maintenance mode only |

**Chose NCS** because it's Nordic's current platform, actively maintained, and all new Nordic silicon is NCS-only. The trade-off is a steeper initial learning curve and longer build times.

---

### Event-Driven Design: Semaphore vs. `k_sleep` vs. Busy-Wait

Chose **`k_sem_take(K_FOREVER)` + `k_timer`** over the alternatives:
- **Busy-wait** — keeps CPU running continuously, no sleep possible. Ruled out immediately.
- **`k_sleep(K_MSEC(n))`** — works, but drift accumulates if the loop body takes variable time. Dynamic interval changes also become awkward.
- **Semaphore + timer** — the timer is the single source of truth for timing, fires at exactly the configured interval regardless of loop body duration. Dynamic changes via `TimerSetInterval()` take effect immediately. Main thread fully suspends between ticks.

---

### Error Handling Strategy

Chose **log + propagate (negative errno)** over the alternatives:
- **`k_panic()` / `__ASSERT`** — too aggressive for init failures on a DK where you want to see the log first.
- **Silent ignore** — a failed `bt_enable()` with no propagation means the device runs with no BLE and no indication why. Unacceptable.

Each module logs the specific error with `LOG_ERR` and returns the raw errno. `main()` decides: critical failures return from `main()` (triggering a Zephyr fault with visible log), non-critical ones log a warning and continue. Consistent with Zephyr's own convention: `0` = success, negative = error.

---

### Temperature: Fixed-Point `int32_t × 100` vs. `float`

Chose **fixed-point `int32_t`** (e.g. 24.92°C = 2492) because:
1. Simpler over BLE — the client just divides by 100, no IEEE 754 decoding needed.
2. Exact integer arithmetic in the filter and min/max — no float rounding surprises.
3. The sensor is only ±0.25°C accurate — two decimal places is plenty.

---

### Custom 128-bit UUIDs vs. BT SIG Standard

The BT SIG Temperature characteristic (0x2A6E) uses `sint16` in 0.01°C units — a different format from this project's `int32_t × 100`. Using the standard UUID with a non-standard format would be misleading. Custom UUIDs make the non-standard encoding explicit and require no collision-avoidance.

---

### Re-advertising: `recycled_cb` vs. `on_disconnected`

`on_disconnected` fires while the BT stack is still cleaning up the connection object — calling `bt_le_adv_start()` there can fail or corrupt state. `recycled_cb` fires only after the connection object is fully freed and back in the pool, making it the correct place to restart advertising. The actual call is also deferred to the work queue to avoid re-entrancy inside the BT callback (see [The System Work Queue](#the-system-work-queue) above).

---

## Watchdog Strategy

The WDT timeout is set to `MAX_INTERVAL_MS × 2` = **20,000 ms**. `wdt_feed()` is called at the bottom of every main loop iteration.

- **Why 2×?** The longest valid sampling interval is 10,000 ms. Doubling gives one full missed tick before reset — catches real hangs without false-firing during normal slow operation.
- **`WDT_FLAG_RESET_SOC`** — resets the entire SoC on timeout, not just the CPU core, so all peripherals including the radio return to a clean state.
- **`WDT_OPT_PAUSE_HALTED_BY_DBG`** — pauses the WDT when the debugger halts the CPU, preventing spurious resets during step-debugging.

---

## Bonus Features

**Moving Average Filter** — 10-sample circular buffer in `temp_sensor.c` smooths ADC noise. Correctly handles warm-up: divides by actual sample count, not the fixed window size.

**Min/Max Tracking** — `TempSensorGetMin()` / `TempSensorGetMax()` track the extremes since boot. Returns `INT32_MAX` / `INT32_MIN` until the first read completes.

**Connection Interval Preference** — requests 100–500 ms connection interval via `bt_conn_le_param_update()` after connecting, reducing radio-on time versus the 7.5 ms Zephyr default. Negotiated values are logged by `on_le_param_updated()`.

**Dynamic Device Name** — derived from BT address at runtime, zero configuration, stable across firmware updates.

**Connection Status LED** — LED2 mirrors BLE connection state in real time.

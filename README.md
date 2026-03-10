# BLE Temperature Node — nRF52840

A BLE peripheral firmware built with **nRF Connect SDK (Zephyr RTOS)** that periodically reads the nRF52840 internal temperature sensor and exposes it over a custom GATT service. Designed for low power: the CPU sleeps between samples, waking only on timer expiry or BLE events.

> **SDK Choice:** This project uses **nRF Connect SDK v3.2.3 (Zephyr RTOS)** — not the legacy nRF5 SDK (SoftDevice).  
> All BLE APIs (`bt_enable`, `bt_gatt_notify`, `BT_GATT_SERVICE_DEFINE`) are from Zephyr's built-in BLE stack.

---

## Table of Contents

- [Getting the Code](#getting-the-code)
- [SDK & Toolchain](#sdk--toolchain)
- [Project Structure](##project-structure)
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

## # Project Structure

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

## Getting the Code

### Clone the Repository

```bash
git clone https://github.com//ble_temp_node.git
cd ble_temp_node
```

> Replace `<your-username>` with your actual GitHub username.

### What You Get

The repository is a self-contained NCS application — no separate SDK repository to clone. The only external dependency is the nRF Connect SDK itself, which you install once via the VS Code extension (see [How to Build & Flash](#how-to-build--flash) below).

```
ble_temp_node/          ← clone this repo
├── src/                ← all application C source files
├── inc/                ← all application headers
├── CMakeLists.txt      ← tells Zephyr's west build system what to compile
├── prj.conf            ← Kconfig options: enables BLE, sensor, WDT, logging
└── README.md
```

### First-Time Setup (one-time only)

1. Install VS Code and the **nRF Connect for VS Code Extension Pack** (see prerequisites in the next section).
2. The extension will ask you to install the **nRF Connect SDK v3.2.3** — do this once. It downloads the full Zephyr tree, toolchain (GCC ARM), and `west` tool automatically. This takes ~5–10 minutes and uses ~5 GB of disk space.
3. After the SDK is installed, open the cloned `ble_temp_node` folder in VS Code. The nRF Connect panel will detect the `CMakeLists.txt` and `prj.conf` and recognise it as an NCS application automatically.

You are now ready to build and flash — no `west init`, no `west update`, no manual PATH configuration required.

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
- Out-of-range values are rejected with ATT error `0x13` (Value Not Allowed).
- The board log confirms: `Application timer interval set to XXXX ms`.

### Disconnect
- Tap **Disconnect**. LED2 turns OFF. The device automatically restarts advertising.

---

## Architecture Overview

### Threads

Zephyr is a preemptive multi-threaded RTOS. In Zephyr, **lower numeric priority = higher urgency**. Priorities split into two domains:
- **Preemptive threads (priority ≥ 0):** can be interrupted by any higher-priority thread at any time.
- **Cooperative threads (priority < 0):** once running, yield voluntarily — they are never preempted mid-execution.

| Thread | Created by | Priority | Type | Role |
|---|---|---|---|---|
| **main** | Zephyr kernel (from `main()`) | 0 | Preemptive | Init sequence, sensor read loop, WDT feed. Blocks on semaphore between ticks — CPU sleeps. |
| **BT RX/TX** | Zephyr BLE stack internally | varies (typically -7 to -4) | Cooperative | Drives all BLE radio events: connections, disconnections, GATT reads/writes, notifications. Calls `on_connected`, `on_disconnected`, `ReadTemperature`, `WriteInterval`. |
| **System work queue** | Zephyr kernel | -1 | Cooperative | Executes deferred work items submitted via `k_work_submit()`. Runs `adv_work_handler` to restart advertising after disconnect. |

#### What does "varies" mean for the BT thread?

The Zephyr BLE stack creates **multiple internal threads** rather than one fixed thread. The exact number and their priorities are controlled by Kconfig symbols inside the BLE stack itself — you do not set them manually. Typical values (NCS v3.x):

| Internal BT thread | Kconfig symbol | Default priority |
|---|---|---|
| `bt_rx` (HCI event/ACL processing) | `CONFIG_BT_RX_PRIO` | -7 |
| `bt_hci_ecc` (ECC crypto, if enabled) | `CONFIG_BT_HCI_ECC_STACK_SIZE` | -7 |
| Controller (if software controller used) | `CONFIG_BT_CTLR_WORKER_PRIO` | -7 |

**Who chooses these priorities?** Nordic and the Zephyr BLE maintainers set the defaults. You can override them in `prj.conf` (e.g. `CONFIG_BT_RX_PRIO=-8`) but this is rarely needed and risks starving other cooperative threads. The defaults are tuned to ensure BLE timing constraints are met while not starving the rest of the system.

**Why are BT threads cooperative (negative priority)?** BLE has hard real-time timing requirements — connection events must be handled within microseconds of their scheduled slot. Cooperative threads guarantee that once the BT stack starts handling a radio event, no lower-priority thread (like `main`, priority 0) can preempt it mid-operation. The BT stack yields control itself when it is done, which is the correct and safe pattern.

**How does this affect your application?** The `main` thread at priority 0 is preemptive and will be interrupted by the BT threads whenever a BLE event arrives. This is why `is_connected`, `is_notifications_enabled`, and `current_temperature` are `atomic_t` — the BT thread can write them at any point while `main` is reading them.

> **Why the work queue for advertising?**  
> `bt_le_adv_start()` cannot be called directly from a BT callback — it would deadlock the BT stack. Submitting it as a `k_work` item defers it to the system work queue thread, which runs outside the BT context.

> **Why `atomic_t` for shared flags?**  
> `is_connected`, `is_notifications_enabled`, and `current_temperature` are written by the BT thread and read by the main thread. `atomic_t` guarantees safe concurrent access without a mutex.

---
### The System Work Queue

The **system work queue** is a Zephyr kernel primitive: a cooperative thread (priority -1) that processes a FIFO queue of `k_work` items. When you call `k_work_submit(&my_work)`, you place a work item onto this queue. The work queue thread picks it up and executes your handler function (`adv_work_handler` in this project) from its own thread context — not from the context where `k_work_submit` was called.

#### Why is this necessary for advertising restart?

When `recycled_cb()` fires (signalling the connection object is freed), it is called **from within the BT stack thread context**. At that moment, the BT stack is still executing. Calling `bt_le_adv_start()` directly from inside a BT callback creates a re-entrancy problem: the BT stack would be calling back into itself, risking a deadlock or corrupted internal state.

The fix is simple: instead of calling `bt_le_adv_start()` directly, submit it as a work item:

```
recycled_cb()                   ← runs inside BT thread
    └─ k_work_submit(&adv_work) ← just adds item to queue, returns immediately
                                   BT thread continues and exits cleanly

System work queue thread        ← runs separately, outside BT context
    └─ adv_work_handler()
           └─ bt_le_adv_start() ← safe to call here
```

This pattern — **defer BT API calls to outside the BT callback context** — is the standard Zephyr BLE pattern for any operation that must happen after a connection event but cannot run inside the callback itself.

> **Why the system work queue and not a new thread?** Creating a dedicated thread for a single function that runs once per reconnect would waste RAM (each Zephyr thread needs its own stack, typically 512–2048 bytes). The system work queue thread already exists and is idle between events — reusing it costs nothing.

### Static Module Diagram

This diagram shows each module, its public interface, and the dependency relationships between modules. Arrows mean "depends on / calls into".

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
| `main.c`         | Orchestration: init sequence, main loop, WDT feed                           |
| `ble_service.c`  | BLE stack init, dynamic device name, GATT service definition, advertising   |
| `temp_sensor.c`  | Sensor read, 10-sample moving average filter, min/max tracking              |
| `app_timer.c`    | Periodic `k_timer` → signals `k_sem` to unblock the main loop               |

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
| BLE events         | Zephyr BLE stack uses the RTC0 peripheral and radio interrupt. Connection, disconnection, and Client Characteristic Configuration writes are all interrupt-driven |
| Watchdog           | WDT interrupt (if callback set) or direct SoC reset |

### How to Measure Current Consumption

#### Option 1 — Nordic PPK2
The **Power Profiler Kit II** is the purpose-built tool for this job. Connect it in **ampere meter mode** between the DK's `VDDMAIN` supply and the board's power input. Use the **nRF Connect Power Profiler** desktop app to record a live current trace. You get µA resolution, timestamps, and a visual breakdown of advertising bursts, connection events, and sleep periods. This is the gold-standard approach for nRF52840 power analysis.

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

| State | What is happening | Duration per cycle | Estimated current | Avg contribution |
|---|---|---|---|---|
| **Deep sleep** | CPU in WFI, radio off, RTC running | ~990 ms / 1000 ms cycle | ~3–5 µA | ~3–5 µA |
| **Sensor read** | CPU wakes, fetches temp via Zephyr driver | ~0.5 ms / cycle | ~1–3 mA peak | ~0.5–1.5 µA |
| **BLE TX burst** | Radio transmits notification packet at 0 dBm | ~1–2 ms / cycle | ~8–10 mA peak | ~8–20 µA |
| **BLE connection event (idle)** | Radio polls at connection interval, no data | ~1 ms per conn interval (100–500 ms) | ~5–8 mA peak | ~10–80 µA |
| **Advertising** (no connection) | 3 ADV PDUs every 100 ms | ~1.5 ms / 100 ms | ~8–10 mA peak | ~120–150 µA |

> **How to read "Avg contribution":** multiply the peak current by the fraction of time in that state.
> Example for BLE TX burst at 1 s interval: 9 mA × (1.5 ms / 1000 ms) ≈ **13.5 µA average contribution**.

### How to Calculate Average Current Consumption

Average current is calculated using the **duty-cycle method**: for each state, multiply its current by the fraction of time spent in it, then sum all contributions.

```
I_avg = Σ ( I_state × T_state ) / T_total
```

#### Example: Connected, notifying at 1 s interval, 100–500 ms connection interval

Assume: 1000 ms cycle, connection interval = 200 ms (5 conn events per cycle),
sensor read = 0.5 ms, BLE TX = 1.5 ms, sleep fills the rest.

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

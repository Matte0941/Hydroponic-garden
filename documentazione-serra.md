# Automated Drip Hydroponic Irrigation System — Documentation

## Overview

This firmware, designed for an Arduino board (ATmega328-based, e.g. Uno/Nano), autonomously manages irrigation for a **drip hydroponic system**: water is delivered from above and drips onto the plant roots, which are supported by **expanded clay pebbles** (LECA) as a free-draining substrate.

Water demand is estimated using the **VPD (Vapor Pressure Deficit)**, an indicator that is more reliable than relative humidity alone for estimating plant water stress. Unlike growing in an absorbent substrate, in a drip system on clay pebbles the water **does not pool**, and the roots are exposed to air between one delivery and the next. For this reason, the system does not vary the duration of each irrigation, but rather **the frequency** at which deliveries repeat, keeping each individual watering event short and constant.

The system is designed to run **without continuous supervision**: it logs everything to an SD card, recovers on its own after a lockup thanks to the hardware watchdog, and tolerates partial sensor failures without stopping.

---

## 1. General architecture

The program is built around a non-blocking `loop()`: no function uses `delay()` — all timing is based on comparisons against `millis()`. This lets the system control several processes at once (sensor reading, pump control, logging, SD monitoring) without any one of them blocking the others.

The processes running in parallel are:

| Process | Interval | Function |
|---|---|---|
| Environmental update (temperature/humidity/VPD/frequency) | every 30 minutes | `aggiornaAmbiente()` |
| SD card presence check | every 1 minute | `controlloSD()` |
| Water level reading | every ~500 ms (10 samples × 50 ms) | `controllaAcqua()` |
| Pump on/off management | continuous (every loop cycle) | `gestionePompa()` |
| Data log writing to SD | every 1 minute | `salvaLog()` |
| Watchdog reset | every loop cycle | `wdt_reset()` |

A key point of the architecture: **pump management is fully independent from the environmental update**. `aggiornaAmbiente()` only updates a single value (`cycleInterval`, the frequency currently in effect); it never touches the pump's state or its timers. This removes at the root a risk present in an earlier version of the firmware, where a mid-cycle recalculation could desynchronize the schedule.

---

## 2. VPD calculation and irrigation frequency

Every 30 minutes the system:

1. Reads temperature and humidity from **two redundant DHT11 sensors** (see section 3).
2. Calculates the saturation vapor pressure (SVP) using the Tetens formula:

   `SVP = 0.6108 × e^(17.27 × T / (T + 237.3))`

3. Calculates the VPD:

   `VPD = SVP × (1 − humidity/100)`

4. Based on the VPD, it determines how many irrigations per hour are needed:
   - If the VPD is below the `VPD_START` threshold, the plant is not under significant water stress → the minimum frequency is used (`IRRIG_MIN_PER_HOUR`, default **7 irrigations/hour**).
   - If the VPD exceeds the threshold, the frequency grows **non-linearly** (the same shape as the previous version's formula, now applied to frequency instead of duration):

     `irrigations/hour = IRRIG_MIN_PER_HOUR + IRRIG_SCALE × (VPD − VPD_START)^1.4`

     The 1.4 exponent means the increase stays modest just above the threshold, then becomes progressively steeper as water stress grows — the same "progressive" behavior as the original formula, moved from duration to frequency.

5. The calculated frequency is then clamped (see section 4) to a safe range before being converted into the time interval between one delivery and the next (`cycleInterval`).

The **duration** of each single irrigation event stays **always fixed** (`PUMP_ON_MIN`, default 3 minutes): what varies with VPD is only how many times per hour this 3-minute delivery repeats. This matches a drip system on clay pebbles, where the goal is to keep the roots periodically moistened without ever leaving them soaked for too long or letting them dry out completely for too long.

---

## 3. Temperature/humidity sensor redundancy

The system reads **two independent DHT11 sensors** connected on pins 7 and 8, handling four cases:

- **Both working** → the average of the two values is used (more stable and resistant to a single anomalous reading).
- **Only one working** → the value from the still-working sensor is used, and an error event (`DHT1_ERROR` or `DHT2_ERROR`) is logged **only at the moment of failure**, not on every cycle, to avoid flooding the log.
- **Both failed** → a single `DHT_BOTH_ERROR` event is logged (again, only once) and the environmental update cycle is aborted: the currently active irrigation frequency is kept as-is, instead of being reset or forced to a default.
- **Recovery** → when a failed sensor comes back online, a dedicated event is logged (`DHT1_RECOVERED` / `DHT2_RECOVERED`).

---

## 4. Pump control — variable-frequency scheduler

The pump (pin 6) is managed as a simple **ON/OFF oscillator** with fixed duration and variable frequency:

- **Fixed duration**: every delivery lasts `PUMP_ON_MIN` (default 3 minutes), always the same, regardless of VPD.
- **Variable frequency**: how often the delivery repeats is determined by `cycleInterval`, calculated in `aggiornaAmbiente()` from the VPD (section 2).
- **No conflict between update and scheduling**: when the pump turns off after 3 minutes, the firmware calculates **at that exact moment** the next start using the most recently available frequency. If the VPD has changed in the meantime, the new rate applies starting from the next cycle, without ever interrupting an ongoing delivery and without needing to "freeze" or reset any state mid-way.
- **Safety limit independent of configuration**: the pause between two deliveries can never drop below `MIN_OFF_TIME` (1 minute, not configurable from file), whatever value is set for `IRRIG_MAX_PER_HOUR`. This prevents a misconfigured `config.txt` parameter from running the pump almost continuously.
- **Water-level safety lockout**: if the reservoir is below the minimum threshold, the pump does not start (or is switched off immediately if already running); the schedule itself is not altered, so the cycle resumes normally as soon as water becomes available again (if the scheduled time has already passed, irrigation starts immediately).
- **Logged events**: every pump on/off transition generates an event (`PUMP_ON` / `PUMP_OFF`) in the event log.

---

## 5. Water level monitoring

The level is read from an analog sensor (pin A0) using a **moving-average filter**: 10 samples collected at 50 ms intervals are averaged before any decision is made, to prevent a single noisy reading from causing unwanted pump on/off transitions.

Three threshold states are handled:

- **Sensor disconnected or faulty** (near-zero reading): the pump is immediately blocked and a `WATER_SENSOR_ERROR` event is logged. This protects against dry running in case of sensor failure.
- **Low level** (`WATER_STOP`): the pump is blocked, `WATER_LOW` event logged.
- **Level restored** (`WATER_OK`): the pump is unblocked, `WATER_OK` event logged.

Using **two distinct thresholds** (`WATER_STOP` lower than `WATER_OK`) instead of a single one creates hysteresis: this prevents the system from continuously oscillating between blocked/unblocked when the water level is near the limit (for example due to sloshing in the tank).

---

## 6. External configuration file

At startup, if the SD card is present, the system reads a `config.txt` file and overrides the default parameters if it finds the following lines (`KEY=value` format):

```
VPD_START=1.0
PUMP_ON_MIN=3
IRRIG_MIN_PER_HOUR=7
IRRIG_MAX_PER_HOUR=15
IRRIG_SCALE=3.0
WATER_STOP=250
WATER_OK=320
```

This allows the system's calibration to be adjusted simply by editing a text file on the SD card, without reflashing the firmware.

> **Safety note**: `IRRIG_MAX_PER_HOUR` represents the *desired* ceiling, but the firmware further limits it if necessary to guarantee at least `MIN_OFF_TIME` (1 minute) of pause between one delivery and the next. If `config.txt` sets a value incompatible with `PUMP_ON_MIN`, the system automatically falls back to the nearest safe maximum, with no need for manual validation.

---

## 7. SD data logging

Two separate CSV files are generated and maintained:

- **`serra.csv`** — periodic log (every minute) of environmental values and status:
  `DATE, TIME, TEMP, HUM, VPD, WATER, PUMP, IRRIG_PER_HOUR`

- **`eventi.csv`** — log of discrete events (pump on/off, sensor errors, system startups, etc.):
  `DATE, TIME, EVENT`

Every row is timestamped using a **DS1307 RTC module**. If the RTC is not detected at startup, the system does not halt: it switches to a fallback mode where rows are marked with `NO_RTC` and the number of seconds elapsed since boot, so the data remains interpretable even without a working clock.

---

## 8. Resilience and automatic recovery

The firmware is designed to run for long periods without human intervention:

- **Hardware watchdog** (`wdt_enable(WDTO_8S)`): if the code locks up for any reason (infinite loop, I2C/SPI lockup, etc.), the board resets itself automatically after 8 seconds without a `wdt_reset()`. On restart, the system recognizes that the reset was caused by the watchdog and logs it as an event (`WATCHDOG_RESET`), useful during maintenance to spot abnormal lockups.
- **Automatic SD recovery**: if the SD card is not detected at startup (perhaps because it wasn't inserted), the system keeps running anyway (simply without saving anything) and retries initialization every minute; as soon as the SD becomes available, the files are created/restored and an `SD_RECOVERED` event is logged.
- **Sensor fault tolerance**: as described, losing one or both DHT11 sensors does not stop the system, which keeps operating on the last valid calculated frequency.
- **Water safety lockout**: under no circumstance can the pump stay on if the water level is below threshold or the sensor appears disconnected.
- **Gradual startup**: on first boot the system does not water immediately, but waits a full interval before the first delivery, so it behaves from the start as if already at steady state instead of an immediate, out-of-schedule watering.

---

## 9. Summary of main constants

| Constant | Default value | Meaning |
|---|---|---|
| `ENV_INTERVAL` | 30 min | Interval between one environmental/frequency recalculation and the next |
| `LOG_INTERVAL` | 1 min | Data log write frequency to SD |
| `MIN_OFF_TIME` | 1 min | Absolute minimum pause between two deliveries (not configurable, safety) |
| `VPD_START` | 1.0 kPa | VPD threshold above which frequency is increased |
| `PUMP_ON_MIN` | 3 min | Fixed duration of each single delivery |
| `IRRIG_MIN_PER_HOUR` | 7 | Minimum irrigation frequency (VPD below threshold) |
| `IRRIG_MAX_PER_HOUR` | 15 | Desired maximum frequency (still limited further by `MIN_OFF_TIME`) |
| `IRRIG_SCALE` | 3.0 | Aggressiveness of frequency growth with VPD (fixed exponent 1.4) |
| `WATER_STOP` | 250 | Threshold below which the pump is blocked |
| `WATER_OK` | 320 | Threshold above which the pump is unblocked |

---

## 10. Summary

The system translates real environmental conditions into a dynamic, safe irrigation frequency, suited to a drip system on expanded clay pebbles where **how often** the roots get wetted matters more than **for how long**. The pump scheduler is decoupled from the environmental recalculation, eliminating timing conflicts, and remains protected by safety limits independent of configuration, with full traceability of every event and measurement via SD logging.

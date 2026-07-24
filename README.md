# Hydroponic Drip Irrigation Controller

Arduino-based firmware for an automated **drip hydroponic irrigation system**. Water drips from above onto roots supported in **expanded clay pebbles (LECA)**. Irrigation frequency is driven by real-time **VPD (Vapor Pressure Deficit)**, calculated from redundant temperature/humidity readings, rather than a fixed timer — keeping delivery duration constant (default 3 minutes) while adapting how often the plants get watered.

Built to run unattended for long periods: SD-card logging, hardware watchdog auto-recovery, sensor fault tolerance, and a water-level safety lockout.

## Features

- **VPD-driven irrigation frequency** — a non-linear formula scales irrigations/hour with plant water stress, with a configurable floor (default 7/hour) and ceiling.
- **Fixed-duration, variable-frequency pump scheduling** — decoupled from the environmental sensing cycle, eliminating scheduling conflicts between the two.
- **Dual DHT11 sensor redundancy** — averages both sensors when healthy, falls back gracefully to a single sensor, and recovers automatically.
- **Water level hysteresis** — dual-threshold logic (`WATER_STOP` / `WATER_OK`) prevents pump chattering near the limit; a disconnected/faulty sensor blocks the pump outright.
- **SD card logging** — periodic environmental log (`serra.csv`) and an event log (`eventi.csv`), both timestamped via a DS1307 RTC with a `NO_RTC` fallback if the clock is unavailable.
- **Field-configurable** — all thresholds and timings are overridable via a `config.txt` file on the SD card, no reflashing required.
- **Hardware watchdog** — automatic reset and recovery from lockups, logged for later diagnosis.

## Hardware

| Component | Connection |
|---|---|
| Arduino Uno/Nano | — |
| 2x DHT11 temperature/humidity sensors | D7, D8 |
| Analog water level sensor | A0 |
| DS1307 RTC module | I2C (A4/A5) |
| SD card module | SPI (D10-D13) |
| Relay module (drives 12V pump) | D6 (signal) |
| 12V water pump | Switched via relay NO contact |
| 12V power supply | Feeds relay/pump directly; feeds Arduino via a 12V to 5V step-down converter |

Wiring diagrams (block-level and formal schematic) are included in the documentation.

## Configuration (`config.txt`)

```
VPD_START=1.0
PUMP_ON_MIN=3
IRRIG_MIN_PER_HOUR=7
IRRIG_MAX_PER_HOUR=15
IRRIG_SCALE=3.0
WATER_STOP=250
WATER_OK=320
```

| Key | Meaning | Default |
|---|---|---|
| `VPD_START` | VPD threshold (kPa) above which irrigation frequency increases | 1.0 |
| `PUMP_ON_MIN` | Fixed duration of each irrigation event (minutes) | 3 |
| `IRRIG_MIN_PER_HOUR` | Minimum irrigations/hour (VPD below threshold) | 7 |
| `IRRIG_MAX_PER_HOUR` | Desired maximum irrigations/hour (further clamped by the 1-minute safety pause) | 15 |
| `IRRIG_SCALE` | Growth rate of frequency with VPD | 3.0 |
| `WATER_STOP` | Analog reading below which the pump is blocked | 250 |
| `WATER_OK` | Analog reading above which the pump is unblocked | 320 |

## Logging

- `serra.csv` - `DATE, TIME, TEMP, HUM, VPD, WATER, PUMP, IRRIG_PER_HOUR` (every minute)
- `eventi.csv` - `DATE, TIME, EVENT` (e.g. `PUMP_ON`, `WATER_LOW`, `DHT1_ERROR`, `SD_RECOVERED`, `WATCHDOG_RESET`)

## Documentation

See [`documentazione_serra.md`](./documentazione_serra.md) for the full technical writeup, including the VPD-to-frequency formula, the pump scheduling model, and failure-recovery behavior.

## License

Add a license of your choice (e.g. MIT) before publishing.

# Astromech Control Engine (ACE)

Real-time control engine for synchronized actuator sequencing: the software brain for an astromech droid build. ACE coordinates motors, lights, and audio against declarative timed sequences, with a swappable actuator backend for simulation now and real hardware later.

Inspired by real R2 builder architecture, ACE follows a Pi-brain / Arduino-actuator split: the C++ core owns scheduling, triggers, telemetry, and sync logic, while Arduino-style slave boards handle direct servo/light I/O.

## Architecture

- **Scheduler** — a monotonic, timed event queue that drives everything else.
- **Trigger mode** — one-shot event responses, compiled through the same execution path as sequences.
- **Sequence mode** — declarative, JSON-defined timed sequences (e.g. dome spin + beep).
- **Actuator backend** — swappable: a virtual backend for simulation and tests, a serial backend for talking to Arduino-style slave boards over UART.
- **Telemetry** — jitter, deadline miss, and sync drift tracking, exposed via CLI and an optional live dashboard.

## Design

See [astromech-control-engine-plan.md](astromech-control-engine-plan.md) for the original design notes.

## Build

```sh
make all
make test
```

The CMake project is also included for environments with CMake installed.

## Run

```sh
./build/ace_cli sequence sequences/dome_spin_beep.seq.json
./build/ace_cli trigger triggers/trigger_map.json panel_3_open
./build/stress_actuators 8 4 2
```

## Dashboard

```sh
cd dashboard
npm install --cache ../work/npm-cache
npm run dev -- --host 127.0.0.1
```

The dashboard starts with simulated telemetry and switches to live data when a WebSocket bridge publishes snapshots at `ws://localhost:8765`.

## Hardware Layer

ACE talks to physical actuators through a serial bridge to an Arduino Mega running a PCA9685 listener sketch. The Mega receives lightweight serial frames from the host and drives hobby servos through the PCA9685 over I2C.

- ACE-side serial implementation: `src/actuator_serial.cpp`
- Arduino listener sketch: `firmware/arduino_ace_slave/arduino_ace_slave.ino`
- Arduino upload notes: `firmware/arduino_ace_slave/README.md`
- Bring-up test script: `scripts/bringup_serial.py`
- Example hardware config: `config/actuators.example.json`
- Wire protocol: `docs/SERIAL_PROTOCOL.md`

Default serial baud rate is `115200` on both ACE and Arduino. `make test` includes a guard to catch baud drift.

### Wire Protocol

The Arduino listener sketch understands four frame types:

- `PING` — USB serial link check.
- `STATUS` — confirms the serial parser and firmware loop are alive.
- `PCASTATUS` — reports PCA9685/I2C readiness.
- `SERVO:<channel>:<angle>` — drives a servo channel to an angle from `0-180`.

### Bring-Up Order

The recommended test order is `PING` → `STATUS` → `PCASTATUS` → one unloaded FS90 servo on channel `0` → one unloaded MG996R servo on channel `1` before connecting panels, drivetrain, or anything with mechanical risk. This isolates serial/link issues from I2C/servo issues before anything with torque is in play.

## Stack

C++20, Make/CMake, React, D3.js, Vite, Arduino C++.

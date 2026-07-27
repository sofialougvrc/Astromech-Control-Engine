# Astromech Control Engine (ACE)

Real-time control engine for synchronized actuator sequencing: the software brain for an astromech droid build. ACE coordinates motors, lights, and audio against declarative timed sequences, with a swappable actuator backend for simulation now and real hardware later.

Inspired by real R2 builder architecture, ACE follows a Pi-brain / Arduino-actuator split: the C++ core owns scheduling, triggers, telemetry, and sync logic, while Arduino-style slave boards eventually handle direct servo/light I/O.

## Status

Working software prototype. Hardware code is drafted but not yet tested against physical devices.

Completed in this repo:

- Monotonic scheduler with timed event queue.
- Trigger mode compiled through the same path as sequence mode.
- Sequence JSON parser and examples.
- Virtual actuator backend for simulation and tests.
- Serial actuator UART write path for Arduino-style slave boards.
- Arduino Mega + PCA9685 listener sketch for `SERVO`, `PING`, and `STATUS` frames.
- Jitter, deadline miss, and sync drift telemetry.
- CLI, benchmark harness, docs, and React/D3 dashboard scaffold.

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

The dashboard starts with simulated telemetry and will switch to live data when a WebSocket bridge publishes snapshots at `ws://localhost:8765`.

## Hardware Drafts

- ACE-side serial implementation: `src/actuator_serial.cpp`
- Arduino listener sketch: `firmware/arduino_ace_slave/arduino_ace_slave.ino`
- Arduino upload notes: `firmware/arduino_ace_slave/README.md`
- Staged laptop bring-up script: `scripts/bringup_serial.py`
- Example hardware config: `config/actuators.example.json`
- Wire protocol: `docs/SERIAL_PROTOCOL.md`

Default serial baud rate is `115200` on both ACE and Arduino. `make test` includes a guard to catch baud drift before hardware testing.

The serial path is written but not hardware-validated. Start with `PING`, then PCA9685 `STATUS`, then one unloaded FS90 on channel `0`, then one unloaded MG996R on channel `1` before connecting panels, drivetrain, or anything with mechanical risk.

## Stack

C++20, Make/CMake, React, D3.js, Vite, Arduino C++.

# ACE Serial Protocol

ACE sends newline-delimited text frames from the Raspberry Pi, Orange Pi, or laptop to the Arduino Mega over USB serial.

Default baud rate is `115200` on both sides:

- ACE: `SerialEndpoint::baud_rate`
- Arduino: `BAUD_RATE` passed to `Serial.begin()`
- Example hardware config: `config/actuators.example.json`

## Frames

```text
PING
STATUS
PCASTATUS
CALSTATUS
TELEMETRY
SERVO:<pca9685-channel>:<angle-deg>
```

Examples:

```text
SERVO:0:90
SERVO:1:135
PING
STATUS
PCASTATUS
CALSTATUS
TELEMETRY
```

The Arduino sketch responds with one line per command:

```text
OK:ACE_PCA9685_READY
OK:ACE_SERIAL_READY
OK:SERVO:0:90:FS90
OK:SERVO:1:135:MG996R
OK:PONG
ERR:PCA9685_INIT
ERR:PCA9685_NOT_READY
ERR:BAD_FRAME
```

`PING` and `STATUS` are serial-layer checks only. They do not depend on PCA9685 readiness and should work even if the servo driver board is unplugged.

`PCASTATUS` is the first hardware-layer readiness check. Use it before sending servo commands.

`CALSTATUS` prints the startup calibration table for the named channels, using:

```text
OK:CAL:<channel>:<profile>:<min-us>:<max-us>:<min-deg>:<max-deg>:<home-deg>
```

`TELEMETRY` prints recent hardware-layer events from the firmware ring buffer,
ending with `OK:TELEMETRY:<count>`.

```text
TEL:<seq>:rx_ms=<received>:exec_ms=<executed>:cmd=<cmd>:status=<status>:ch=<channel>:req=<requested-angle>:applied=<applied-angle>:pulse_us=<pulse>:ticks=<pca9685-off-tick>:i2c=<wire-status>
OK:TELEMETRY:<count>
```

`rx_ms` and `exec_ms` come from Arduino `millis()`. `i2c=255` means the command
did not touch I2C, which is expected for `PING` and `STATUS`.

## Failure Policy

ACE and the bridge intentionally fail loud on hardware-layer errors:

- Malformed serial frame -> `ERR:BAD_FRAME`; record telemetry with `status=BAD_FRAME`.
- Unknown command -> `ERR:UNKNOWN_COMMAND`; record telemetry with `status=UNKNOWN_COMMAND`.
- Bad servo channel -> `ERR:SERVO_CHANNEL`; do not move anything.
- Non-numeric servo angle -> `ERR:SERVO_ANGLE`; do not move anything.
- Numeric angle outside the calibrated range -> clamp to the channel profile range, respond `OK:SERVO`, and record telemetry with `status=CLAMPED`.
- PCA9685 missing at startup or no I2C ACK -> `ERR:PCA9685_INIT` from `PCASTATUS`.
- PCA9685 unavailable during servo command -> `ERR:PCA9685_NOT_READY` or `ERR:PCA9685_I2C`; do not continue motion.
- Serial ACK timeout or bridge `ERR:*` while ACE is running `SerialActuator` -> throw a C++ exception and stop the current sequence.
- Serial connection drop mid-sequence -> next ACK read times out or the write fails; ACE stops instead of chaining later servo commands.

PCA9685 servo channels are zero-based and match the labels on the driver board: `0` through `15`.

Out-of-range angle values are clamped to the servo profile's configured range.

## ACE Mapping

`SerialActuator` converts high-level `ActuatorCommand` objects into serial frames:

- `type=servo`, `angle_deg=90` -> `SERVO:<channel>:90`
- `type=servo`, `action=open`, `open_angle_deg=105` -> `SERVO:<channel>:105`

The channel comes from `command.params["channel"]` first, then from `SerialEndpoint::channel`.

See `config/actuators.example.json` for the intended actuator-to-device mapping shape.

## Servo Profiles

The Arduino firmware keeps per-channel servo profiles instead of assuming one universal pulse range:

- Channel `0`: FEETECH FS90, datasheet range `900-2100us` across `0-120` degrees
- Channel `1`: MG996R, starts at `500-2500us`
- Channels `2-15`: generic 180 degree profile, starts at `600-2400us`

These are conservative bench-test values. Servo batches vary, especially with generic MG996R-compatible parts. If a servo buzzes, binds, or hits an end stop before the requested angle, narrow that profile's pulse range before mounting the servo in a mechanism.

Calibration data lives in two places:

- Firmware startup table: `firmware/arduino_ace_slave/servo_calibration.hpp`
- Laptop/fake bridge JSON: `config/servo_calibration.example.json`

The fake bridge loads the JSON at startup. The Arduino sketch loads its calibration table during `setup()` into the runtime `servoProfiles` array.

At `50Hz`, one PCA9685 PWM period is about `20,000us`. The PCA9685 divides that period into `4096` ticks, so one tick is about `4.88us`. The firmware converts angle to pulse microseconds first, then converts microseconds to PCA9685 ticks.

## Wiring Summary

```text
Mega 5V       -> PCA9685 VCC
Mega GND      -> PCA9685 GND
Mega SDA      -> PCA9685 SDA
Mega SCL      -> PCA9685 SCL

6V supply +   -> PCA9685 V+
6V supply -   -> PCA9685 GND
Capacitor +   -> PCA9685 V+
Capacitor -   -> PCA9685 GND
```

Do not power servos from the Mega. Do not connect the 6V servo supply to the Mega 5V pin. Mega ground and servo power ground must be common.

## Staged Bring-Up Script

After uploading the firmware, run the host-side test from the repo root:

```sh
python3 scripts/bringup_serial.py /dev/tty.usbmodem1101
```

The script stops immediately on the first unexpected response. Its order is:

1. `PING`: proves USB serial and baud rate.
2. `STATUS`: proves serial parsing/responding without touching PCA9685.
3. `PCASTATUS`: proves the PCA9685 initialized over I2C.
4. `SERVO:0:60`, `SERVO:0:0`, `SERVO:0:120`: moves the low-torque FS90 first.
5. `TELEMETRY`: prints recent command timing, servo pulse, tick, and I2C status.
6. Optional MG996R test:

```sh
python3 scripts/bringup_serial.py /dev/tty.usbmodem1101 --mg996r
```

## Fake PCA9685 Bridge

Before hardware arrives, run the same protocol against a laptop-only fake bridge:

```sh
python3 scripts/fake_pca9685_bridge.py --symlink /tmp/ace_fake_pca9685
```

In another terminal, run the bring-up script against the fake serial device:

```sh
python3 scripts/bringup_serial.py /tmp/ace_fake_pca9685
```

Or run an ACE sequence through the real C++ `SerialActuator` path:

```sh
make all
./build/ace_cli sequence-serial sequences/examples/fs90_sweep.seq.json /tmp/ace_fake_pca9685
```

The fake bridge logs the `setPWM(channel, 0, ticks)` call it would have sent to
the PCA9685, including the servo profile, clamped angle, pulse width, and final
12-bit tick value.

If you only want to verify the fake bridge protocol and pulse math:

```sh
python3 scripts/fake_pca9685_bridge.py --self-test
```

To rehearse failure modes without hardware:

```sh
# PCA9685 missing / no I2C ACK
python3 scripts/fake_pca9685_bridge.py --pca-unavailable

# I2C fails after the first successful servo write
python3 scripts/fake_pca9685_bridge.py --fail-after-servo-writes 1

# Serial bridge disappears after three received frames
python3 scripts/fake_pca9685_bridge.py --drop-after-frames 3
```

## Current Hardware Caveat

This protocol compiles now, but it has not been tested against a physical Arduino yet. First hardware validation should be a single LED or unloaded servo before connecting panels, drive motors, or anything with mechanical risk.

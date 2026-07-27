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
SERVO:<pca9685-channel>:<angle-deg>
```

Examples:

```text
SERVO:0:90
SERVO:1:135
PING
STATUS
PCASTATUS
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

PCA9685 servo channels are zero-based and match the labels on the driver board: `0` through `15`.

Out-of-range angle values are clamped to the servo profile's configured range. For the current bench setup that is `0-180`.

## ACE Mapping

`SerialActuator` converts high-level `ActuatorCommand` objects into serial frames:

- `type=servo`, `angle_deg=90` -> `SERVO:<channel>:90`
- `type=servo`, `action=open`, `open_angle_deg=105` -> `SERVO:<channel>:105`

The channel comes from `command.params["channel"]` first, then from `SerialEndpoint::channel`.

See `config/actuators.example.json` for the intended actuator-to-device mapping shape.

## Servo Profiles

The Arduino firmware keeps per-channel servo profiles instead of assuming one universal pulse range:

- Channel `0`: FEETECH FS90, starts at `500-2400us`
- Channel `1`: MG996R, starts at `500-2500us`
- Channels `2-15`: generic 180 degree profile, starts at `600-2400us`

These are conservative bench-test values. Servo batches vary, especially with generic MG996R-compatible parts. If a servo buzzes, binds, or hits an end stop before the requested angle, narrow that profile's pulse range before mounting the servo in a mechanism.

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
4. `SERVO:0:90`, `SERVO:0:0`, `SERVO:0:180`: moves the low-torque FS90 first.
5. Optional MG996R test:

```sh
python3 scripts/bringup_serial.py /dev/tty.usbmodem1101 --mg996r
```

## Current Hardware Caveat

This protocol compiles now, but it has not been tested against a physical Arduino yet. First hardware validation should be a single LED or unloaded servo before connecting panels, drive motors, or anything with mechanical risk.

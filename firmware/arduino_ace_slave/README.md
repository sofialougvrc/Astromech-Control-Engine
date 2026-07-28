# ACE Arduino PCA9685 Slave

This sketch runs on an Arduino Mega 2560 Rev3 and drives servos through a PCA9685 16-channel PWM/servo driver over I2C.

## Arduino IDE Dependencies

Install these libraries through Arduino Library Manager:

- `Adafruit PWM Servo Driver Library`
- `Adafruit BusIO`

## Upload Target

- Board: Arduino Mega or Mega 2560
- Baud used by ACE protocol: `115200`
- PCA9685 I2C address: `0x40`
- Servo PWM frequency: `50Hz`
- Calibration table: `servo_calibration.hpp`

## PlatformIO

Open this folder in VS Code:

```text
firmware/arduino_ace_slave
```

PlatformIO should detect `platformio.ini`. Use the `megaatmega2560` environment, then run:

```sh
pio run
pio run --target upload
pio device monitor --baud 115200
```

The checked-in `src/main.cpp` includes the root `.ino` file so the same sketch can be used from both Arduino IDE and PlatformIO.

## First Bench Test

After upload, open Serial Monitor at `115200` baud with newline line endings and send:

```text
PING
STATUS
PCASTATUS
CALSTATUS
TELEMETRY
SERVO:0:60
SERVO:0:0
SERVO:0:120
SERVO:1:90
```

Expected responses:

```text
OK:PONG
OK:ACE_SERIAL_READY
OK:ACE_PCA9685_READY
OK:CAL:0:FS90:900:2100:0:120:60
TEL:1:rx_ms=1200:exec_ms=1201:cmd=PING:status=OK:ch=-1:req=-1:applied=-1:pulse_us=0:ticks=0:i2c=255
OK:TELEMETRY:1
OK:SERVO:0:60:FS90
OK:SERVO:1:90:MG996R
```

`PING` and `STATUS` only prove the USB serial command loop. `PCASTATUS` is the first I2C/PCA9685 check. Keep those stages separate so a wiring problem does not look like a serial problem.

## Laptop Bring-Up Script

From the repo root:

```sh
python3 scripts/bringup_serial.py /dev/tty.usbmodem1101
```

This runs the serial checks, then PCA9685 readiness, then the FS90 channel `0` test. It stops on the first failure.

Run the higher-torque MG996R channel `1` test only when the servo is unloaded and you are ready:

```sh
python3 scripts/bringup_serial.py /dev/tty.usbmodem1101 --mg996r
```

## Channel Profiles

- Channel `0`: FEETECH FS90, `900-2100us`, range `0-120`, home `60`
- Channel `1`: MG996R, `500-2500us`, home `90`
- Channels `2-15`: generic 180 degree servo, `600-2400us`, home `90`

These are first-pass calibration values. If a servo buzzes, binds, or pushes into its physical stop, narrow that channel's pulse range in `servo_calibration.hpp`.

## Failure Behavior

- `PING` and `STATUS` stay serial-only.
- `PCASTATUS` reports PCA9685/I2C readiness.
- Malformed frames return `ERR:BAD_FRAME`.
- Bad servo channels return `ERR:SERVO_CHANNEL`.
- Non-numeric servo angles return `ERR:SERVO_ANGLE`.
- Out-of-range numeric angles are clamped and logged as `CLAMPED`.
- I2C failures return `ERR:PCA9685_INIT`, `ERR:PCA9685_NOT_READY`, or `ERR:PCA9685_I2C`.
- `TELEMETRY` prints the recent command log so bring-up debugging has timestamps and status codes.

# Hardware

Parts list for the physical build. Software status lives in the main [README](README.md); this file tracks the hardware side.

## Controller

**Arduino Mega 2560 Rev3**
Official Arduino board built on the ATmega2560 running at 16 MHz. Provides 54 digital I/O pins (15 PWM-capable), 16 analog inputs, and 4 hardware UARTs. Runs the firmware that bridges serial commands from ACE to the PCA9685.

## Servo Driver

**PCA9685 16-Channel 12-bit PWM/Servo Driver (I2C)**
I2C-controlled PWM driver board offering 16 independent channels of 12-bit-resolution PWM output. Takes servo-control load off the Mega's own pins/timers — the Mega sends channel/angle targets over I2C, and the PCA9685 generates the actual PWM signal to each servo.

## Servos

**FEETECH FS90 Micro Servo** — channel 0
9g analog servo with roughly 120° of travel, using pulse widths of 900–2100µs. Used as the low-torque, low-risk servo for initial bring-up testing and small mechanisms.

**MG996R 180° Metal Gear Servo** — channel 1
Digital servo with metal gearing and a full 180° range, offering more torque and rotational range than typical hobby servos. Used for panels, doors, or mechanisms needing stronger, more precise positional control.

## Power

**6VDC 5A Power Supply (center-positive)**
Dedicated regulated power source for the servo rail, separate from the Arduino's own power. Includes protection against over-current, over-voltage, and short-circuits, with an on/off switch and a center-positive barrel connector.

**RS PRO 1000µF 35V Capacitor**
Electrolytic capacitor placed across the servo power rail (V+/GND) to smooth current draw spikes when servos start/stop moving, reducing the risk of voltage sag or brownouts on the shared supply.

## Enclosure

**3D Printer** — not yet selected
Needed to print Artoo's exterior shell (dome, body panels, and structural mounts for the servos and mechanisms), to be printed and bench-fitted after electronics are validated.

## Wiring notes

- Servo power rail and Arduino power are kept separate; grounds are tied together (common ground).
- Capacitor sits directly across the PCA9685's V+/GND terminals.
- Power supply polarity is center-positive — confirm before connecting any barrel jack adapter.

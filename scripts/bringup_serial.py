#!/usr/bin/env python3
"""
Staged ACE hardware bring-up test.

Run this from a laptop after the Arduino Mega has been flashed. The order is
deliberate: prove the USB serial loop first, then prove the PCA9685 is alive,
then move the low-torque FS90, and only then optionally test the MG996R.
"""

import argparse
import sys
import time

try:
    import serial
except ImportError:
    print("Missing dependency: pyserial")
    print("Install it with: python3 -m pip install pyserial")
    sys.exit(2)


BAUD_RATE = 115200
READ_TIMEOUT_SECONDS = 2.0


def send_command(port, command):
    """Send one newline-delimited command and read one response line."""
    port.reset_input_buffer()
    print(f"> {command}")
    port.write((command + "\n").encode("ascii"))
    port.flush()

    response = port.readline().decode("ascii", errors="replace").strip()
    if not response:
        raise RuntimeError(f"No response for {command!r}")

    print(f"< {response}")
    return response


def require_response(response, expected, stage):
    """Stop immediately if the current stage did not return the expected line."""
    if response != expected:
        raise RuntimeError(f"{stage} failed: expected {expected!r}, got {response!r}")


def require_ok_prefix(response, prefix, stage):
    """Stop immediately unless the board clearly reports an OK for this stage."""
    if not response.startswith(prefix):
        raise RuntimeError(f"{stage} failed: expected prefix {prefix!r}, got {response!r}")


def print_telemetry(port):
    """Ask the bridge for its recent hardware-layer timing and I2C log."""
    print("> TELEMETRY")
    port.reset_input_buffer()
    port.write(b"TELEMETRY\n")
    port.flush()

    while True:
        line = port.readline().decode("ascii", errors="replace").strip()
        if not line:
            raise RuntimeError("No response while reading TELEMETRY")
        print(f"< {line}")
        if line.startswith("OK:TELEMETRY:"):
            return


def run_fs90_sequence(port):
    # Channel 0 is the low-torque FS90 bench-test servo. These raw positional
    # moves intentionally avoid easing/smoothing so you can observe the hardware
    # and calibration directly before building fancier motion on top.
    for command, expected_prefix in [
        ("SERVO:0:90", "OK:SERVO:0:90:"),
        ("SERVO:0:0", "OK:SERVO:0:0:"),
        ("SERVO:0:180", "OK:SERVO:0:180:"),
    ]:
        response = send_command(port, command)
        require_ok_prefix(response, expected_prefix, "FS90 servo test")
        time.sleep(0.7)


def run_mg996r_sequence(port):
    # Channel 1 is the higher-torque MG996R. It is intentionally separated from
    # the automatic test path so you consciously choose to energize it.
    for command, expected_prefix in [
        ("SERVO:1:90", "OK:SERVO:1:90:"),
        ("SERVO:1:45", "OK:SERVO:1:45:"),
        ("SERVO:1:135", "OK:SERVO:1:135:"),
        ("SERVO:1:90", "OK:SERVO:1:90:"),
    ]:
        response = send_command(port, command)
        require_ok_prefix(response, expected_prefix, "MG996R servo test")
        time.sleep(0.9)


def main():
    parser = argparse.ArgumentParser(description="Run staged ACE Arduino/PCA9685 bring-up checks.")
    parser.add_argument("port", help="Serial port, for example /dev/tty.usbmodem1101 or COM3")
    parser.add_argument("--mg996r", action="store_true", help="Also run the explicit high-torque MG996R test")
    args = parser.parse_args()

    print("ACE hardware bring-up")
    print("Stage 1 checks only USB serial receive/parse/respond.")
    print("Stage 2 checks PCA9685 I2C readiness before any servo moves.")
    print()

    with serial.Serial(args.port, BAUD_RATE, timeout=READ_TIMEOUT_SECONDS) as port:
        # Give the Mega time to reset after the serial port opens. Many Arduino
        # boards auto-reset on USB serial open, so the first boot line may arrive
        # before we send commands.
        time.sleep(2.0)
        port.reset_input_buffer()

        ping = send_command(port, "PING")
        require_response(ping, "OK:PONG", "serial PING")

        status = send_command(port, "STATUS")
        require_response(status, "OK:ACE_SERIAL_READY", "serial STATUS")

        pca_status = send_command(port, "PCASTATUS")
        require_response(pca_status, "OK:ACE_PCA9685_READY", "PCA9685 status")

        print()
        print("Serial and PCA9685 checks passed. Testing FS90 on channel 0.")
        run_fs90_sequence(port)

        if args.mg996r:
            print()
            print("Running explicit MG996R test on channel 1.")
            run_mg996r_sequence(port)
        else:
            print()
            print("Skipping MG996R high-torque test.")
            print("Run again with --mg996r when the servo is unloaded and you are ready.")

        print()
        print("Recent hardware-layer telemetry:")
        print_telemetry(port)

    print()
    print("Bring-up test completed successfully.")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nStopped by user.")
        sys.exit(130)
    except Exception as error:
        print(f"\nFAILED: {error}")
        sys.exit(1)

#!/usr/bin/env python3
"""
Laptop-only fake Arduino Mega + PCA9685 bridge.

This exposes a pseudo serial port, accepts the same frames as the Arduino
firmware, and logs the PCA9685 calls it would have made. It lets ACE exercise:

  ACE scheduler -> SerialActuator UART frames -> firmware protocol -> servo math

without an Arduino, PCA9685, or servos attached.
"""

import argparse
import os
import pty
import select
import signal
import sys
import tty
import time


BAUD_RATE = 115200
PCA9685_SERVO_HZ = 50
PCA9685_TICKS = 4096
PCA9685_CHANNELS = 16

SERVO_PROFILES = {
    0: ("FS90", 500, 2400, 0, 180),
    1: ("MG996R", 500, 2500, 0, 180),
}
GENERIC_PROFILE = ("GENERIC_180", 600, 2400, 0, 180)


def clamp(value, low, high):
    return max(low, min(high, value))


def servo_profile(channel):
    return SERVO_PROFILES.get(channel, GENERIC_PROFILE)


def angle_to_pulse_us(angle_deg, profile):
    _, min_pulse, max_pulse, min_angle, max_angle = profile
    clamped_angle = clamp(angle_deg, min_angle, max_angle)
    span_in = max_angle - min_angle
    span_out = max_pulse - min_pulse
    pulse = min_pulse + ((clamped_angle - min_angle) * span_out / span_in)
    return clamped_angle, round(pulse)


def pulse_us_to_ticks(pulse_us):
    period_us = 1_000_000 / PCA9685_SERVO_HZ
    return clamp(round((pulse_us * PCA9685_TICKS) / period_us), 0, PCA9685_TICKS - 1)


def parse_int(text):
    if not text or not text.lstrip("+-").isdigit():
        return None
    return int(text)


class FakePca9685:
    def __init__(self, log_path=None):
        self.log_path = log_path
        self.events = []

    def set_servo_angle(self, channel, requested_angle):
        profile = servo_profile(channel)
        name, _, _, _, _ = profile
        applied_angle, pulse_us = angle_to_pulse_us(requested_angle, profile)
        ticks = pulse_us_to_ticks(pulse_us)

        event = {
            "timestamp": time.time(),
            "channel": channel,
            "profile": name,
            "requested_angle": requested_angle,
            "applied_angle": applied_angle,
            "pulse_us": pulse_us,
            "ticks": ticks,
        }
        self.events.append(event)
        self.log(event)
        return applied_angle, name

    def log(self, event):
        line = (
            f"PCA9685 setPWM channel={event['channel']} on=0 off={event['ticks']} "
            f"profile={event['profile']} requested={event['requested_angle']} "
            f"applied={event['applied_angle']} pulse_us={event['pulse_us']}"
        )
        print(line, flush=True)
        if self.log_path:
            with open(self.log_path, "a", encoding="utf-8") as output:
                output.write(line + "\n")


def handle_frame(frame, fake_pca):
    frame = frame.strip()
    if not frame:
        return None

    if frame == "PING":
        return "OK:PONG"
    if frame == "STATUS":
        return "OK:ACE_SERIAL_READY"
    if frame == "PCASTATUS":
        return "OK:ACE_PCA9685_READY"

    parts = frame.split(":")
    if len(parts) != 3 or parts[0] != "SERVO":
        return "ERR:BAD_FRAME"

    channel = parse_int(parts[1])
    if channel is None or channel < 0 or channel >= PCA9685_CHANNELS:
        return "ERR:SERVO_CHANNEL"

    angle = parse_int(parts[2])
    if angle is None:
        return "ERR:SERVO_ANGLE"

    applied_angle, profile_name = fake_pca.set_servo_angle(channel, angle)
    return f"OK:SERVO:{channel}:{applied_angle}:{profile_name}"


def install_symlink(target, symlink_path):
    if os.path.lexists(symlink_path):
        os.unlink(symlink_path)
    os.symlink(target, symlink_path)


def main():
    parser = argparse.ArgumentParser(description="Run a fake ACE PCA9685 serial bridge.")
    parser.add_argument("--symlink", default="/tmp/ace_fake_pca9685", help="Stable serial-port symlink to create")
    parser.add_argument("--log", help="Optional file for fake PCA9685 setPWM logs")
    parser.add_argument("--self-test", action="store_true", help="Run protocol/math checks without opening a pseudo-terminal")
    args = parser.parse_args()

    if args.self_test:
        fake_pca = FakePca9685()
        checks = [
            ("PING", "OK:PONG"),
            ("STATUS", "OK:ACE_SERIAL_READY"),
            ("PCASTATUS", "OK:ACE_PCA9685_READY"),
            ("SERVO:0:90", "OK:SERVO:0:90:FS90"),
            ("SERVO:0:-20", "OK:SERVO:0:0:FS90"),
            ("SERVO:1:200", "OK:SERVO:1:180:MG996R"),
            ("SERVO:16:90", "ERR:SERVO_CHANNEL"),
            ("SERVO:0:nope", "ERR:SERVO_ANGLE"),
        ]
        for frame, expected in checks:
            actual = handle_frame(frame, fake_pca)
            if actual != expected:
                raise RuntimeError(f"{frame!r}: expected {expected!r}, got {actual!r}")
        print("fake PCA9685 bridge self-test: ok")
        return

    master_fd, slave_fd = pty.openpty()
    tty.setraw(slave_fd)
    slave_name = os.ttyname(slave_fd)
    install_symlink(slave_name, args.symlink)

    fake_pca = FakePca9685(args.log)
    buffer = bytearray()
    running = True

    def stop(_signum, _frame):
        nonlocal running
        running = False

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)

    print("Fake ACE PCA9685 bridge ready.", flush=True)
    print(f"Serial port: {slave_name}", flush=True)
    print(f"Stable link: {args.symlink}", flush=True)
    print(f"Baud: {BAUD_RATE}", flush=True)
    print("Use this path as the ACE serial device. Press Ctrl-C to stop.", flush=True)
    print(flush=True)

    try:
        while running:
            readable, _, _ = select.select([master_fd], [], [], 0.2)
            if not readable:
                continue

            data = os.read(master_fd, 256)
            if not data:
                continue

            for byte in data:
                if byte == ord("\r"):
                    continue
                if byte == ord("\n"):
                    frame = buffer.decode("ascii", errors="replace")
                    buffer.clear()
                    response = handle_frame(frame, fake_pca)
                    if response:
                        print(f"< {frame.strip()}", flush=True)
                        print(f"> {response}", flush=True)
                        os.write(master_fd, (response + "\n").encode("ascii"))
                    continue

                if len(buffer) < 96:
                    buffer.append(byte)
                else:
                    buffer.clear()
                    os.write(master_fd, b"ERR:LINE_TOO_LONG\n")
    finally:
        if os.path.islink(args.symlink):
            os.unlink(args.symlink)
        os.close(master_fd)
        os.close(slave_fd)


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"FAILED: {error}", file=sys.stderr)
        sys.exit(1)

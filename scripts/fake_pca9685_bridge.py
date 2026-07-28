#!/usr/bin/env python3
"""
Laptop-only fake Arduino Mega + PCA9685 bridge.

This exposes a pseudo serial port, accepts the same frames as the Arduino
firmware, and logs the PCA9685 calls it would have made. It lets ACE exercise:

  ACE scheduler -> SerialActuator UART frames -> firmware protocol -> servo math

without an Arduino, PCA9685, or servos attached.
"""

import argparse
import json
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

DEFAULT_CALIBRATION_PATH = "config/servo_calibration.example.json"
DEFAULT_PROFILE = ("GENERIC_180", 600, 2400, 0, 180, 90)


def clamp(value, low, high):
    return max(low, min(high, value))


def angle_to_pulse_us(angle_deg, profile):
    _, min_pulse, max_pulse, min_angle, max_angle, _ = profile
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
    def __init__(self, calibration, log_path=None, pca_ready=True, fail_after_servo_writes=None):
        self.calibration = calibration
        self.log_path = log_path
        self.pca_ready = pca_ready
        self.fail_after_servo_writes = fail_after_servo_writes
        self.servo_writes = 0
        self.events = []
        self.telemetry = []
        self.sequence = 0
        self.started_at = time.monotonic()

    def next_sequence(self):
        self.sequence += 1
        return self.sequence

    def millis(self):
        return round((time.monotonic() - self.started_at) * 1000)

    def set_servo_angle(self, channel, requested_angle):
        if not self.pca_ready:
            raise RuntimeError("PCA9685_NOT_READY")
        if self.fail_after_servo_writes is not None and self.servo_writes >= self.fail_after_servo_writes:
            self.pca_ready = False
            raise RuntimeError("PCA9685_I2C")

        profile = self.calibration.profile_for(channel)
        name, _, _, _, _, _ = profile
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
        self.servo_writes += 1
        self.events.append(event)
        self.log(event)
        return applied_angle, name

    def record_telemetry(self, sequence, received_at_ms, executed_at_ms, command, status, channel=-1, requested_angle=-1, applied_angle=-1, pulse_us=0, ticks=0, i2c_status=255):
        line = (
            f"TEL:{sequence}:rx_ms={received_at_ms}:exec_ms={executed_at_ms}:"
            f"cmd={command}:status={status}:ch={channel}:req={requested_angle}:"
            f"applied={applied_angle}:pulse_us={pulse_us}:ticks={ticks}:i2c={i2c_status}"
        )
        self.telemetry.append(line)
        self.telemetry = self.telemetry[-16:]

    def telemetry_response(self):
        return "\n".join([*self.telemetry, f"OK:TELEMETRY:{len(self.telemetry)}"])

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


class ServoCalibration:
    def __init__(self, default_profile=DEFAULT_PROFILE, channel_profiles=None):
        self.default_profile = default_profile
        self.channel_profiles = channel_profiles or {}

    @classmethod
    def from_file(cls, path):
        with open(path, "r", encoding="utf-8") as input_file:
            data = json.load(input_file)

        default_profile = parse_profile(data["default_profile"])
        channel_profiles = {}
        for entry in data.get("channels", []):
            channel_profiles[int(entry["channel"])] = parse_profile(entry["profile"])

        return cls(default_profile=default_profile, channel_profiles=channel_profiles)

    def profile_for(self, channel):
        return self.channel_profiles.get(channel, self.default_profile)


def parse_profile(profile):
    return (
        profile["name"],
        int(profile["min_pulse_us"]),
        int(profile["max_pulse_us"]),
        int(profile["min_angle_deg"]),
        int(profile["max_angle_deg"]),
        int(profile.get("home_angle_deg", 90)),
    )


def handle_frame(frame, fake_pca):
    received_at_ms = fake_pca.millis()
    sequence = fake_pca.next_sequence()
    frame = frame.strip()
    if not frame:
        return None

    if frame == "PING":
        fake_pca.record_telemetry(sequence, received_at_ms, fake_pca.millis(), "PING", "OK")
        return "OK:PONG"
    if frame == "STATUS":
        fake_pca.record_telemetry(sequence, received_at_ms, fake_pca.millis(), "STATUS", "OK")
        return "OK:ACE_SERIAL_READY"
    if frame == "PCASTATUS":
        fake_pca.record_telemetry(sequence, received_at_ms, fake_pca.millis(), "PCASTATUS", "OK" if fake_pca.pca_ready else "I2C_ERR", i2c_status=0 if fake_pca.pca_ready else 2)
        return "OK:ACE_PCA9685_READY" if fake_pca.pca_ready else "ERR:PCA9685_INIT"
    if frame == "CALSTATUS":
        lines = []
        for channel in sorted(fake_pca.calibration.channel_profiles):
            name, min_pulse, max_pulse, min_angle, max_angle, home_angle = fake_pca.calibration.profile_for(channel)
            lines.append(f"OK:CAL:{channel}:{name}:{min_pulse}:{max_pulse}:{min_angle}:{max_angle}:{home_angle}")
        fake_pca.record_telemetry(sequence, received_at_ms, fake_pca.millis(), "CALSTATUS", "OK")
        return "\n".join(lines)
    if frame == "TELEMETRY":
        fake_pca.record_telemetry(sequence, received_at_ms, fake_pca.millis(), "TELEMETRY", "OK")
        return fake_pca.telemetry_response()

    parts = frame.split(":")
    if len(parts) != 3 or parts[0] != "SERVO":
        fake_pca.record_telemetry(sequence, received_at_ms, fake_pca.millis(), "PARSE", "BAD_FRAME")
        return "ERR:BAD_FRAME"

    channel = parse_int(parts[1])
    if channel is None or channel < 0 or channel >= PCA9685_CHANNELS:
        fake_pca.record_telemetry(sequence, received_at_ms, fake_pca.millis(), "SERVO", "BAD_CHANNEL", channel=channel if channel is not None else -1)
        return "ERR:SERVO_CHANNEL"

    angle = parse_int(parts[2])
    if angle is None:
        fake_pca.record_telemetry(sequence, received_at_ms, fake_pca.millis(), "SERVO", "BAD_ANGLE", channel=channel)
        return "ERR:SERVO_ANGLE"

    if not fake_pca.pca_ready:
        fake_pca.record_telemetry(sequence, received_at_ms, fake_pca.millis(), "SERVO", "PCA9685_NOT_READY", channel=channel, requested_angle=angle, i2c_status=2)
        return "ERR:PCA9685_NOT_READY"

    profile = fake_pca.calibration.profile_for(channel)
    applied_angle, pulse_us = angle_to_pulse_us(angle, profile)
    ticks = pulse_us_to_ticks(pulse_us)
    try:
        applied_angle, profile_name = fake_pca.set_servo_angle(channel, angle)
    except RuntimeError:
        fake_pca.record_telemetry(sequence, received_at_ms, fake_pca.millis(), "SERVO", "I2C_ERR", channel=channel, requested_angle=angle, i2c_status=2)
        return "ERR:PCA9685_I2C"
    fake_pca.record_telemetry(sequence, received_at_ms, fake_pca.millis(), "SERVO", "OK" if angle == applied_angle else "CLAMPED", channel=channel, requested_angle=angle, applied_angle=applied_angle, pulse_us=pulse_us, ticks=ticks, i2c_status=0)
    return f"OK:SERVO:{channel}:{applied_angle}:{profile_name}"


def install_symlink(target, symlink_path):
    if os.path.lexists(symlink_path):
        os.unlink(symlink_path)
    os.symlink(target, symlink_path)


def main():
    parser = argparse.ArgumentParser(description="Run a fake ACE PCA9685 serial bridge.")
    parser.add_argument("--symlink", default="/tmp/ace_fake_pca9685", help="Stable serial-port symlink to create")
    parser.add_argument("--calibration", default=DEFAULT_CALIBRATION_PATH, help="Servo calibration JSON to load at startup")
    parser.add_argument("--pca-unavailable", action="store_true", help="Simulate a missing/non-ACKing PCA9685")
    parser.add_argument("--fail-after-servo-writes", type=int, help="Simulate I2C failure after N successful servo writes")
    parser.add_argument("--drop-after-frames", type=int, help="Close the pseudo-port after N received frames")
    parser.add_argument("--log", help="Optional file for fake PCA9685 setPWM logs")
    parser.add_argument("--self-test", action="store_true", help="Run protocol/math checks without opening a pseudo-terminal")
    args = parser.parse_args()
    calibration = ServoCalibration.from_file(args.calibration)

    if args.self_test:
        fake_pca = FakePca9685(calibration)
        checks = [
            ("PING", "OK:PONG"),
            ("STATUS", "OK:ACE_SERIAL_READY"),
            ("PCASTATUS", "OK:ACE_PCA9685_READY"),
            ("CALSTATUS", "OK:CAL:0:FS90:500:2400:0:180:90\nOK:CAL:1:MG996R:500:2500:0:180:90"),
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
        telemetry = handle_frame("TELEMETRY", fake_pca)
        if "TEL:" not in telemetry or not telemetry.endswith("OK:TELEMETRY:10"):
            raise RuntimeError(f"unexpected telemetry response: {telemetry!r}")

        failing_pca = FakePca9685(calibration, pca_ready=False)
        failure_checks = [
            ("PCASTATUS", "ERR:PCA9685_INIT"),
            ("SERVO:0:90", "ERR:PCA9685_NOT_READY"),
            ("TELEMETRY", None),
        ]
        for frame, expected in failure_checks:
            actual = handle_frame(frame, failing_pca)
            if expected is not None and actual != expected:
                raise RuntimeError(f"{frame!r}: expected {expected!r}, got {actual!r}")
        if "status=PCA9685_NOT_READY" not in failing_pca.telemetry_response():
            raise RuntimeError("missing PCA failure telemetry")

        mid_sequence_failure = FakePca9685(calibration, fail_after_servo_writes=1)
        if handle_frame("SERVO:0:90", mid_sequence_failure) != "OK:SERVO:0:90:FS90":
            raise RuntimeError("first mid-sequence servo write should pass")
        if handle_frame("SERVO:0:45", mid_sequence_failure) != "ERR:PCA9685_I2C":
            raise RuntimeError("second mid-sequence servo write should simulate I2C failure")
        if "status=I2C_ERR" not in mid_sequence_failure.telemetry_response():
            raise RuntimeError("missing mid-sequence I2C failure telemetry")
        print("fake PCA9685 bridge self-test: ok")
        return

    master_fd, slave_fd = pty.openpty()
    tty.setraw(slave_fd)
    slave_name = os.ttyname(slave_fd)
    install_symlink(slave_name, args.symlink)

    fake_pca = FakePca9685(
        calibration,
        args.log,
        pca_ready=not args.pca_unavailable,
        fail_after_servo_writes=args.fail_after_servo_writes,
    )
    buffer = bytearray()
    running = True
    frames_seen = 0

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
                    frames_seen += 1
                    if args.drop_after_frames is not None and frames_seen > args.drop_after_frames:
                        print("Simulating serial connection drop.", flush=True)
                        running = False
                        break
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

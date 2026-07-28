#!/usr/bin/env python3
import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
JSON_PATH = ROOT / "config" / "servo_calibration.example.json"
HEADER_PATH = ROOT / "firmware" / "arduino_ace_slave" / "servo_calibration.hpp"


def profile_tuple(profile):
    return (
        profile["name"],
        int(profile["min_pulse_us"]),
        int(profile["max_pulse_us"]),
        int(profile["min_angle_deg"]),
        int(profile["max_angle_deg"]),
        int(profile["home_angle_deg"]),
    )


def header_profile(name, text):
    pattern = rf'{name}\s*=\s*\{{"([^"]+)",\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\}};'
    match = re.search(pattern, text)
    if not match:
        raise RuntimeError(f"Missing {name} in servo_calibration.hpp")
    profile_name, *numbers = match.groups()
    return (profile_name, *(int(number) for number in numbers))


def main():
    data = json.loads(JSON_PATH.read_text())
    header = HEADER_PATH.read_text()

    expected_default = profile_tuple(data["default_profile"])
    expected_fs90 = profile_tuple(data["channels"][0]["profile"])
    expected_mg996r = profile_tuple(data["channels"][1]["profile"])

    actual_default = header_profile("DEFAULT_SERVO_PROFILE", header)
    actual_fs90 = header_profile("FS90_SERVO_PROFILE", header)
    actual_mg996r = header_profile("MG996R_SERVO_PROFILE", header)

    if expected_default != actual_default:
        raise RuntimeError(f"default calibration mismatch: json={expected_default} firmware={actual_default}")
    if expected_fs90 != actual_fs90:
        raise RuntimeError(f"FS90 calibration mismatch: json={expected_fs90} firmware={actual_fs90}")
    if expected_mg996r != actual_mg996r:
        raise RuntimeError(f"MG996R calibration mismatch: json={expected_mg996r} firmware={actual_mg996r}")

    print("servo calibration check: ok")


if __name__ == "__main__":
    main()

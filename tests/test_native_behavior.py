"""Host-compiled behavioral integration tests against production sources.

This pytest harness is a second line of defense. The authoritative native
PlatformIO path is `python -m platformio test -e native`, configured in CI.
"""
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

SOURCES = [
    "lib/native_stubs/src/arduino_stubs.cpp",
    "lib/native_stubs/src/config_test_stub.cpp",
    "Servo_flute_ESP32/ConfigValidator.cpp",
    "Servo_flute_ESP32/PressureController.cpp",
    "Servo_flute_ESP32/EventQueue.cpp",
    "Servo_flute_ESP32/FingerController.cpp",
    "Servo_flute_ESP32/AirflowController.cpp",
    "Servo_flute_ESP32/FanController.cpp",
    "Servo_flute_ESP32/NoteSequencer.cpp",
    "Servo_flute_ESP32/InstrumentManager.cpp",
    "tests/cpp/test_behavior.cpp",
]


def test_cpp_behavioral_production_sources(tmp_path):
    binary = tmp_path / "behavior"
    cmd = [
        "g++",
        "-std=c++17",
        "-DUNIT_TEST",
        "-Ilib/native_stubs/include",
        "-IServo_flute_ESP32",
        *SOURCES,
        "-o",
        str(binary),
    ]
    subprocess.run(cmd, cwd=ROOT, check=True)
    result = subprocess.run([str(binary)], cwd=ROOT, text=True, capture_output=True, check=True)
    assert "behavior tests passed" in result.stdout

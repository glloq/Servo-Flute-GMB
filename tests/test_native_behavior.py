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
    "Servo_flute_ESP32/ConfigDefaults.cpp",
    "Servo_flute_ESP32/ConfigPersist.cpp",
    "Servo_flute_ESP32/ConfigTopology.cpp",
    "Servo_flute_ESP32/ConfigValidator.cpp",
    "Servo_flute_ESP32/ConfigCommit.cpp",
    "Servo_flute_ESP32/WebAuth.cpp",
    "Servo_flute_ESP32/Sha256.cpp",
    "Servo_flute_ESP32/PressureController.cpp",
    "Servo_flute_ESP32/TofSensor.cpp",
    "Servo_flute_ESP32/AudioRingBuffer.cpp",
    "Servo_flute_ESP32/SpectralAnalyzer.cpp",
    "Servo_flute_ESP32/AudioFilters.cpp",
    "Servo_flute_ESP32/NoiseModel.cpp",
    "Servo_flute_ESP32/AcousticFeatures.cpp",
    "Servo_flute_ESP32/AcousticQuality.cpp",
    "Servo_flute_ESP32/AcousticTiming.cpp",
    "Servo_flute_ESP32/EventQueue.cpp",
    "Servo_flute_ESP32/CommandQueue.cpp",
    "Servo_flute_ESP32/FingerController.cpp",
    "Servo_flute_ESP32/AirflowController.cpp",
    "Servo_flute_ESP32/FanController.cpp",
    "Servo_flute_ESP32/NoteSequencer.cpp",
    "Servo_flute_ESP32/InstrumentManager.cpp",
    "Servo_flute_ESP32/CalibrationAirSupply.cpp",
    "Servo_flute_ESP32/AutoCalibrator.cpp",
    "Servo_flute_ESP32/PitchDetector.cpp",
    "Servo_flute_ESP32/MidiFilePlayer.cpp",
    # General-Midi-Boop recognition: the pure core (GmbRuntime.cpp is the ESP32
    # glue - eFuse MAC + NVS - and is not part of the host build).
    "Servo_flute_ESP32/gmb/Capabilities.cpp",
    "Servo_flute_ESP32/gmb/GmbDescriptor.cpp",
    "Servo_flute_ESP32/gmb/GmbInstanceId.cpp",
    "Servo_flute_ESP32/gmb/GmbMidiBridge.cpp",
    "Servo_flute_ESP32/gmb/GmbRevision.cpp",
    "Servo_flute_ESP32/gmb/GmbSysEx.cpp",
    "Servo_flute_ESP32/gmb/GmbSysExService.cpp",
    "tests/test_native/test_gmb.cpp",
    "tests/test_native/test_audit.cpp",
    "tests/test_native/test_audio.cpp",
    "tests/test_native/test_spectral_hnr.cpp",
    "tests/test_native/test_quality.cpp",
    "tests/test_native/test_timing.cpp",
    "tests/test_native/test_hw_pump.cpp",
    "tests/test_native/test_hw_note.cpp",
    "tests/test_native/test_hw_servo.cpp",
    "tests/test_native/test_hw_boot.cpp",
    "tests/test_native/test_harden_storage.cpp",
    "tests/test_native/test_harden_commit.cpp",
    "tests/test_native/test_harden_autocal.cpp",
    "tests/test_native/test_harden_memory.cpp",
    "tests/test_native/test_harden_tasks.cpp",
    "tests/test_native/test_fin_actuators.cpp",
    "tests/test_native/test_fin_storage.cpp",
    "tests/test_native/test_behavior.cpp",
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

"""Regression tests for the semantic GMB v2 ``messages`` capability block.

The block must describe what Servo-Flute actually executes, not what a MIDI
transport parser can merely decode. These tests compile the production
descriptor serializer and parse its output with Python's JSON parser.
"""

import json
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]

SOURCES = [
    "lib/native_stubs/src/arduino_stubs.cpp",
    "lib/native_stubs/src/config_test_stub.cpp",
    "Servo_flute_ESP32/ConfigValidator.cpp",
    "Servo_flute_ESP32/gmb/Capabilities.cpp",
    "Servo_flute_ESP32/gmb/GmbDescriptor.cpp",
    "Servo_flute_ESP32/gmb/GmbInstanceId.cpp",
    "Servo_flute_ESP32/gmb/GmbSysEx.cpp",
    "tests/tools/gmb_descriptor_dump.cpp",
]


@pytest.fixture(scope="module")
def descriptors(tmp_path_factory):
    binary = tmp_path_factory.mktemp("gmb-messages") / "dump"
    subprocess.run(
        [
            "g++",
            "-std=c++17",
            "-DUNIT_TEST",
            "-Ilib/native_stubs/include",
            "-IServo_flute_ESP32",
            *SOURCES,
            "-o",
            str(binary),
        ],
        cwd=ROOT,
        check=True,
    )
    out = subprocess.run(
        [str(binary)], cwd=ROOT, text=True, capture_output=True, check=True
    ).stdout

    parsed = {}
    label = None
    for line in out.splitlines():
        if line.startswith("### "):
            label = line[4:].strip()
        elif line.startswith("DESCRIPTOR "):
            parsed[label] = json.loads(line[len("DESCRIPTOR ") :])

    assert set(parsed) == {"contiguous", "discrete", "long-escaped", "unconfigured"}
    return parsed


def test_configured_flutes_publish_semantic_message_support(descriptors):
    expected = {
        "note_on": True,
        "note_off": True,
        "control_change": True,
        "program_change": False,
        "pitch_bend": False,
        "channel_aftertouch": False,
        "poly_aftertouch": False,
        "clock": False,
        "start": False,
        "continue": False,
        "stop": False,
        "system_reset": False,
    }

    for label in ("contiguous", "discrete", "long-escaped"):
        inst = descriptors[label]["instruments"][0]
        assert inst["configured"] is True
        assert inst["messages"] == expected


def test_messages_agree_with_expression_capabilities(descriptors):
    for label in ("contiguous", "discrete", "long-escaped"):
        inst = descriptors[label]["instruments"][0]
        messages = inst["messages"]
        expression = inst["expression"]

        assert messages["control_change"] is bool(expression["cc"])
        assert messages["pitch_bend"] is expression["pitch_bend"]["supported"]
        assert messages["channel_aftertouch"] is expression["channel_aftertouch"]
        assert messages["poly_aftertouch"] is expression["poly_aftertouch"]


def test_unconfigured_instrument_does_not_claim_message_capabilities(descriptors):
    inst = descriptors["unconfigured"]["instruments"][0]
    assert inst == {"channel": 0, "configured": False}
    assert "messages" not in inst

"""Independent checks of the General-Midi-Boop descriptor and handshake.

The C++ suite (tests/test_native/test_gmb.cpp) asserts on individual fields with
a hand-rolled reader. This one runs the production serialiser and then parses its
output with a real JSON parser, and decodes the handshake and the block 0x10
segments exactly the way General-Midi-Boop does
(src/midi/devices/DeviceManager.js, src/midi/instrument/DescriptorProtocol.js),
so an encoding mistake cannot pass both.
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

# GMB vocabulary: type/subtype are keys of src/midi/adaptation/InstrumentTypeConfig.js.
PIPE_SUBTYPES = {
    "piccolo": 72, "flute": 73, "recorder": 74, "pan_flute": 75,
    "bottle": 76, "shakuhachi": 77, "whistle": 78, "ocarina": 79,
}


def _dec32(b):
    """DeviceManager.parseGmbHandshake's 32-bit little-endian 7-bit decode."""
    return ((b[0] & 0x7F) | ((b[1] & 0x7F) << 7) | ((b[2] & 0x7F) << 14)
            | ((b[3] & 0x7F) << 21) | ((b[4] & 0x0F) << 28)) & 0xFFFFFFFF


@pytest.fixture(scope="module")
def cases(tmp_path_factory):
    binary = tmp_path_factory.mktemp("gmb") / "dump"
    subprocess.run(
        ["g++", "-std=c++17", "-DUNIT_TEST", "-Ilib/native_stubs/include",
         "-IServo_flute_ESP32", *SOURCES, "-o", str(binary)],
        cwd=ROOT, check=True,
    )
    out = subprocess.run([str(binary)], cwd=ROOT, text=True,
                         capture_output=True, check=True).stdout
    parsed = {}
    label = None
    for line in out.splitlines():
        if line.startswith("### "):
            label = line[4:].strip()
            parsed[label] = {"chunks": []}
        elif line.startswith("DESCRIPTOR "):
            parsed[label]["descriptor"] = line[len("DESCRIPTOR "):]
        elif line.startswith("HANDSHAKE "):
            parsed[label]["handshake"] = bytes.fromhex(line[len("HANDSHAKE "):])
        elif line.startswith("CHUNK "):
            parsed[label]["chunks"].append(bytes.fromhex(line[len("CHUNK "):]))
    assert set(parsed) == {"contiguous", "discrete", "long-escaped", "unconfigured"}
    return parsed


def test_descriptor_is_valid_json_and_pure_ascii(cases):
    for label, case in cases.items():
        text = case["descriptor"]
        assert text.isascii(), f"{label}: descriptor is not 7-bit ASCII"
        doc = json.loads(text)              # a real parser, not our own reader
        assert doc["gmb_descriptor"] == 2
        assert isinstance(doc["revision"], int) and doc["revision"] >= 0
        assert doc["device"]["model"] == "Servo-Flute-GMB"
        assert len(doc["instruments"]) == 1, f"{label}: one logical instrument"
        inst = doc["instruments"][0]
        assert 0 <= inst["channel"] <= 15


def test_configured_instrument_announces_a_real_flute(cases):
    for label in ("contiguous", "discrete", "long-escaped"):
        inst = json.loads(cases[label]["descriptor"])["instruments"][0]
        assert inst["configured"] is True
        assert inst["type"] == "pipe"
        assert inst["subtype"] in PIPE_SUBTYPES
        assert inst["gm_program"] == PIPE_SUBTYPES[inst["subtype"]]
        assert inst["polyphony"] == {"max": 1}
        assert inst["physical"]["family"] == "winds"
        # No string-instrument field is forced into a wind instrument.
        assert not {"tuning", "fret_count", "string_count"} & set(inst["physical"])
        timing = inst["timing"]
        # Silent finger positioning stays in `prepare` so GMB can anticipate it.
        assert timing["prepare"]["silent"] is True
        assert timing["prepare"]["max_ms"] == 105
        # An unknown value is omitted, never announced as 0.
        assert "release_ms" not in timing
        assert 0 not in [v for v in timing.values() if isinstance(v, int)]


def test_acoustic_excite_latency_is_never_guessed(cases):
    """`timing.excite.latency_ms` is the delay until the note is AUDIBLE.

    General-Midi-Boop aligns instruments on it, so a value the firmware has not
    measured is worse than no value: the GMB rule is that an absent field means
    unknown. Nothing here measures the acoustic onset - in particular
    `solenoidActivationTimeMs` is the solenoid's full-power drive window, not the
    moment the air column speaks - so the key must be absent everywhere.
    """
    for label, case in cases.items():
        text = case["descriptor"]
        assert "excite" not in text, f"{label}: excite announced without a measurement"
        assert "latency_ms" not in text, f"{label}: a latency is announced"
        inst = json.loads(text)["instruments"][0]
        timing = inst.get("timing", {})
        assert "excite" not in timing
        # And above all it is not announced as an instant attack.
        assert timing.get("excite", {}).get("latency_ms") != 0


def test_note_modes_match_the_configured_fingerings(cases):
    contiguous = json.loads(cases["contiguous"]["descriptor"])["instruments"][0]
    assert contiguous["notes"] == {"mode": "range", "min": 72, "max": 84}

    discrete = json.loads(cases["discrete"]["descriptor"])["instruments"][0]
    assert discrete["notes"]["mode"] == "discrete"
    notes = discrete["notes"]["list"]
    assert notes == [82, 83, 84, 86, 88, 89, 91, 93, 95, 96, 98, 100, 101, 103]
    assert notes == sorted(set(notes))
    # A discrete list is only used when the set really has gaps.
    assert len(notes) != notes[-1] - notes[0] + 1


def test_unconfigured_instrument_announces_nothing_else(cases):
    inst = json.loads(cases["unconfigured"]["descriptor"])["instruments"][0]
    assert inst["configured"] is False
    assert set(inst) == {"channel", "configured"}


def test_handshake_agrees_with_the_descriptor(cases):
    for label, case in cases.items():
        m = case["handshake"]
        assert len(m) == 24, f"{label}: handshake must be exactly 24 bytes"
        assert m[0:5] == bytes([0xF0, 0x7D, 0x00, 0x01, 0x01])
        assert m[5] == 0x02, "proto_ver must be 2"
        assert m[23] == 0xF7
        assert all(b < 0x80 for b in m[1:23]), "payload must be 7-bit safe"
        instance_id = _dec32(m[6:11])
        assert instance_id != 0, "instance_id {0,0,0,0,0} is non-conformant"
        assert (m[11], m[12], m[13]) == (1, 1, 0)
        size = (m[14] & 0x7F) | ((m[15] & 0x7F) << 7) | ((m[16] & 0x7F) << 14)
        assert size == len(case["descriptor"].encode("ascii"))
        assert _dec32(m[17:22]) == json.loads(case["descriptor"])["revision"]
        assert m[22] == 0x03


def test_block_10_segments_reassemble_exactly(cases):
    for label, case in cases.items():
        chunks = case["chunks"]
        assert chunks, f"{label}: at least one segment"
        rebuilt = ""
        for i, c in enumerate(chunks):
            assert c[0:5] == bytes([0xF0, 0x7D, 0x00, 0x10, 0x01])
            assert c[-1] == 0xF7
            assert all(b < 0x80 for b in c[1:-1])
            total = (c[5] & 0x7F) | ((c[6] & 0x7F) << 7)
            index = (c[7] & 0x7F) | ((c[8] & 0x7F) << 7)
            assert total == len(chunks)
            assert index == i
            payload = c[9:-1]
            assert len(payload) <= 200, "payload cap keeps the frame at 210 bytes"
            if i + 1 < total:
                assert len(payload) == 200
            rebuilt += payload.decode("ascii")
        assert rebuilt == case["descriptor"]
        json.loads(rebuilt)

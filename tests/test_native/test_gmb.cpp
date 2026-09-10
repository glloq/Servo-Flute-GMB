// Host tests for the General-Midi-Boop v2 recognition and capability reporting
// implementation (Servo_flute_ESP32/gmb, docs/GMB_PROTOCOL.md).
//
// Everything here runs against the production sources; the only thing stubbed is
// the platform (Arduino.h / the `cfg` global), exactly as for test_behavior.cpp.
// The ESP32 glue (GmbRuntime.cpp: eFuse MAC + NVS) is not part of this build - the
// decisions it delegates to (instance id derivation, revision persistence) are
// pure and are tested directly.

#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "Arduino.h"
#include "ConfigStorage.h"
#include "gmb/Capabilities.h"
#include "gmb/GmbDescriptor.h"
#include "gmb/GmbInstanceId.h"
#include "gmb/GmbMidiBridge.h"
#include "gmb/GmbRevision.h"
#include "gmb/GmbSysEx.h"
#include "gmb/GmbSysExService.h"

using namespace gmb;

// ---------------------------------------------------------------------------
// Minimal JSON reader: enough to assert on the descriptor without pulling a
// parser into the host build. Returns the raw text of a value (object, array,
// string or number) for a "key" at any depth, searching from `from`.
// ---------------------------------------------------------------------------
namespace {

size_t jsonFind(const std::string& j, const std::string& key, size_t from = 0) {
  const std::string needle = "\"" + key + "\":";
  return j.find(needle, from);
}

bool jsonHas(const std::string& j, const std::string& key) {
  return jsonFind(j, key) != std::string::npos;
}

std::string jsonValue(const std::string& j, const std::string& key, size_t from = 0) {
  size_t p = jsonFind(j, key, from);
  if (p == std::string::npos) return std::string();
  p += key.size() + 3;   // "key":
  if (p >= j.size()) return std::string();
  const char c = j[p];
  if (c == '"') {
    size_t end = p + 1;
    while (end < j.size() && j[end] != '"') {
      if (j[end] == '\\') end++;
      end++;
    }
    return j.substr(p, end - p + 1);
  }
  if (c == '{' || c == '[') {
    const char open = c, close = (c == '{') ? '}' : ']';
    int depth = 0;
    size_t end = p;
    bool inStr = false;
    for (; end < j.size(); end++) {
      if (inStr) {
        if (j[end] == '\\') { end++; continue; }
        if (j[end] == '"') inStr = false;
        continue;
      }
      if (j[end] == '"') { inStr = true; continue; }
      if (j[end] == open) depth++;
      else if (j[end] == close) { depth--; if (depth == 0) break; }
    }
    return j.substr(p, end - p + 1);
  }
  size_t end = p;
  while (end < j.size() && j[end] != ',' && j[end] != '}' && j[end] != ']') end++;
  return j.substr(p, end - p);
}

// Structural JSON validity: balanced braces/brackets outside strings, and a
// non-empty document that starts with '{' and ends with '}'.
bool jsonWellFormed(const std::string& j) {
  if (j.size() < 2 || j.front() != '{' || j.back() != '}') return false;
  std::vector<char> stack;
  bool inStr = false;
  for (size_t i = 0; i < j.size(); i++) {
    const char c = j[i];
    if (inStr) {
      if (c == '\\') { i++; continue; }
      if (c == '"') inStr = false;
      continue;
    }
    if (c == '"') { inStr = true; continue; }
    if (c == '{' || c == '[') stack.push_back(c);
    else if (c == '}') { if (stack.empty() || stack.back() != '{') return false; stack.pop_back(); }
    else if (c == ']') { if (stack.empty() || stack.back() != '[') return false; stack.pop_back(); }
  }
  return !inStr && stack.empty();
}

// ---- configuration fixtures ----------------------------------------------

// A minimal but valid flute configuration: 6 fingers, `count` notes starting at
// `firstNote`, contiguous semitones, all with a usable airflow window.
void makeConfig(uint8_t count, uint8_t firstNote, uint8_t step) {
  memset(&cfg, 0, sizeof(cfg));
  cfg.numFingers = 6;
  cfg.numNotes = count;
  cfg.airflowPcaChannel = 10;
  cfg.fingerAngleOpen = 30;
  cfg.halfHolePercent = 50;
  strncpy(cfg.embouchure, "trav", sizeof(cfg.embouchure) - 1);
  for (int i = 0; i < 6; i++) {
    cfg.fingers[i].pcaChannel = (uint8_t)i;
    cfg.fingers[i].closedAngle = 90;
    cfg.fingers[i].direction = -1;
  }
  for (int i = 0; i < count; i++) {
    cfg.notes[i].midiNote = (uint8_t)(firstNote + i * step);
    cfg.notes[i].airflowMinPercent = 10;
    cfg.notes[i].airflowMaxPercent = 60;
    cfg.notes[i].airflowNominalPercent = 30;
    cfg.notes[i].anglePercent = 50;
  }
  cfg.midiChannel = 0;                // omni
  cfg.servoToSolenoidDelayMs = 105;
  cfg.minNoteIntervalForValveCloseMs = 50;
  cfg.minNoteDurationMs = 10;
  cfg.servoAirflowOff = 20;
  cfg.servoAirflowMin = 60;
  cfg.servoAirflowMax = 100;
  cfg.servoAngleOff = 90;
  cfg.servoAngleMin = 60;
  cfg.servoAngleMax = 120;
  cfg.vibratoFrequencyHz = 6.0f;
  cfg.vibratoMaxAmplitudeDeg = 8.0f;
  cfg.cc2Enabled = true;
  cfg.cc2SilenceThreshold = 10;
  cfg.cc2ResponseCurve = 1.4f;
  cfg.cc2TimeoutMs = 1000;
  cfg.solenoidPwmActivation = 255;
  cfg.solenoidPwmHolding = 128;
  cfg.solenoidActivationTimeMs = 50;
  cfg.airVelocityResponse = 60;
  cfg.airMode = AIR_MODE_SOLENOID_SERVO;
  cfg.valveType = 0;
  cfg.solenoidPin = SOLENOID_PIN;
  cfg.angleServoEnabled = false;
  cfg.numPumps = 1;
  strncpy(cfg.deviceName, "ServoFlute", sizeof(cfg.deviceName) - 1);
}

CapabilitySnapshot snapshotOf(uint32_t revision, uint32_t instanceId, bool validated = true) {
  CapabilitySnapshot s = buildSnapshot(cfg, validated);
  s.revision = revision;
  s.identity.instanceId = instanceId;
  return s;
}

std::vector<uint8_t> handshakeRequest() {
  return std::vector<uint8_t>{0xF0, 0x7D, 0x00, 0x01, 0x00, 0xF7};
}

std::vector<uint8_t> chunkRequest(uint16_t index) {
  return std::vector<uint8_t>{0xF0, 0x7D, 0x00, 0x10, 0x00,
                              (uint8_t)(index & 0x7F), (uint8_t)((index >> 7) & 0x7F), 0xF7};
}

// Every byte between F0 and F7 must be 7-bit for the frame to survive a MIDI wire.
bool payload7BitSafe(const std::vector<uint8_t>& m) {
  for (size_t i = 1; i + 1 < m.size(); i++) {
    if (m[i] & 0x80) return false;
  }
  return true;
}

// The GMB-side decoders, reimplemented here from
// General-Midi-Boop/src/midi/devices/DeviceManager.js, so the tests assert
// against the controller's actual reading of the frame rather than against our
// own encoder.
uint32_t gmbDecode32(const uint8_t* b) {
  return ((uint32_t)(b[0] & 0x7F)) | ((uint32_t)(b[1] & 0x7F) << 7) |
         ((uint32_t)(b[2] & 0x7F) << 14) | ((uint32_t)(b[3] & 0x7F) << 21) |
         ((uint32_t)(b[4] & 0x0F) << 28);
}

// A port that records what it is given, standing in for BLE / rtpMIDI.
struct FakePort : public IGmbMidiPort {
  bool connected = true;
  std::vector<std::vector<uint8_t> > sent;
  const char* name = "fake";
  bool canSendSysEx() const override { return connected; }
  void sendSysEx(const uint8_t* data, size_t len) override {
    sent.push_back(std::vector<uint8_t>(data, data + len));
  }
  const char* gmbPortName() const override { return name; }
};

// ---------------------------------------------------------------------------
// 1. Instance id / 7-bit codecs
// ---------------------------------------------------------------------------
void gmb_instance_id_codec() {
  const uint32_t values[] = {0u, 1u, 127u, 128u, 0x0FFFFFFFu, 0x80000000u,
                             0xFFFFFFFFu, 0xDEADBEEFu, 0x12345678u};
  for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
    uint8_t enc[5];
    encode32Le7(values[i], enc);
    for (int b = 0; b < 5; b++) assert((enc[b] & 0x80) == 0);   // 7-bit safe
    assert(decode32Le7(enc) == values[i]);                       // round trip
    assert(gmbDecode32(enc) == values[i]);                       // GMB reads the same
  }
  // Bit 31 must survive: a 3-bit high byte (the legacy v1 codec) would halve the
  // identifier space and silently collide two boards.
  uint8_t hi[5];
  encode32Le7(0x80000000u, hi);
  assert(hi[4] == 0x08);
  assert(decode32Le7(hi) == 0x80000000u);

  const uint32_t sizes[] = {0u, 1u, 200u, 4095u, 0x1FFFFFu};
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
    uint8_t enc[3];
    encode21Le7(sizes[i], enc);
    for (int b = 0; b < 3; b++) assert((enc[b] & 0x80) == 0);
    assert(decode21Le7(enc) == sizes[i]);
  }
}

void gmb_instance_id_is_stable_and_unique() {
  const uint64_t macA = 0x0000A4CF12345678ull;
  const uint64_t macB = 0x0000A4CF12345679ull;   // neighbouring board
  uint32_t a = instanceIdFromHardwareId(macA);
  uint32_t b = instanceIdFromHardwareId(macB);
  assert(a == instanceIdFromHardwareId(macA));   // stable across "reboots"
  assert(a != b);                                // different between two boards
  assert(a != 0 && b != 0);                      // never the non-conformant {0,...}
  // Two MACs differing only in bit 7 of a byte must still differ: a plain 7-bit
  // truncation of the MAC would map them onto the same identity.
  assert(instanceIdFromHardwareId(0x0000A4CF12345600ull) !=
         instanceIdFromHardwareId(0x0000A4CF12345680ull));
  // The id does not depend on the user-visible instrument name.
  makeConfig(4, 60, 1);
  CapabilitySnapshot s1 = snapshotOf(1, a);
  strncpy(cfg.deviceName, "Autre nom", sizeof(cfg.deviceName) - 1);
  CapabilitySnapshot s2 = snapshotOf(1, instanceIdFromHardwareId(macA));
  assert(s1.identity.instanceId == s2.identity.instanceId);
}

// ---------------------------------------------------------------------------
// 2. Block 1 handshake
// ---------------------------------------------------------------------------
void gmb_handshake_frame() {
  makeConfig(14, 60, 1);
  GmbSysExService svc;
  svc.setSnapshot(snapshotOf(42, 0x9ABCDEF0u));
  svc.setHttpDescriptorAvailable(true);

  std::vector<uint8_t> req = handshakeRequest();
  std::vector<uint8_t> m = svc.handleMessage(req.data(), req.size(), 1000);

  assert(m.size() == 24);                       // exactly 24 bytes
  assert(m[0] == 0xF0 && m[1] == 0x7D && m[2] == 0x00);   // header
  assert(m[3] == 0x01 && m[4] == 0x01);         // block 1, response
  assert(m[5] == 0x02);                         // proto_ver = 2
  assert(gmbDecode32(&m[6]) == 0x9ABCDEF0u);    // stable instance id
  assert(m[6] != 0 || m[7] != 0 || m[8] != 0 || m[9] != 0 || m[10] != 0);
  assert(m[11] == FIRMWARE_VERSION_MAJOR);      // firmware from the single source
  assert(m[12] == FIRMWARE_VERSION_MINOR);
  assert(m[13] == FIRMWARE_VERSION_PATCH);
  uint32_t announcedSize = decode21Le7(&m[14]);
  assert(announcedSize == svc.descriptorJson().size());   // matches the real document
  assert(announcedSize > 0);
  assert(gmbDecode32(&m[17]) == 42u);           // revision
  assert(m[22] == (kHttpDescriptorAvailable | kChangeNotificationSupported));
  assert(m[23] == 0xF7);                        // termination
  assert(payload7BitSafe(m));

  // Without a web server, bit 0 is not announced.
  svc.setHttpDescriptorAvailable(false);
  m = svc.handleMessage(req.data(), req.size(), 1010);
  assert(m.size() == 24);
  assert(m[22] == kChangeNotificationSupported);
}

// ---------------------------------------------------------------------------
// 3. Descriptor content
// ---------------------------------------------------------------------------
void gmb_descriptor_contiguous_range() {
  makeConfig(13, 72, 1);            // C5..C6, every semitone
  std::string j = GmbDescriptor::toJson(snapshotOf(7, 0x11112222u));

  assert(jsonWellFormed(j));
  assert(jsonValue(j, "gmb_descriptor") == "2");
  assert(jsonValue(j, "revision") == "7");
  assert(jsonValue(j, "model") == "\"Servo-Flute-GMB\"");
  assert(jsonValue(j, "configured") == "true");
  assert(jsonValue(j, "channel") == "0");
  assert(jsonValue(j, "gm_program") == "73");           // transverse flute
  assert(jsonValue(j, "type") == "\"pipe\"");            // InstrumentTypeConfig key
  assert(jsonValue(j, "subtype") == "\"flute\"");
  std::string notes = jsonValue(j, "notes");
  assert(notes == "{\"mode\":\"range\",\"min\":72,\"max\":84}");
  assert(jsonValue(j, "polyphony") == "{\"max\":1}");

  // Exactly one instrument entry.
  assert(j.find("\"instruments\":[{") != std::string::npos);
  assert(j.find("},{") == std::string::npos);
}

void gmb_descriptor_discrete_notes() {
  makeConfig(8, 60, 2);             // whole tones: not contiguous
  std::string j = GmbDescriptor::toJson(snapshotOf(1, 1));
  assert(jsonWellFormed(j));
  assert(jsonValue(j, "notes") ==
         "{\"mode\":\"discrete\",\"list\":[60,62,64,66,68,70,72,74]}");
}

void gmb_descriptor_excludes_disabled_fingerings() {
  makeConfig(5, 60, 1);             // 60,61,62,63,64
  cfg.notes[2].airflowMaxPercent = 0;    // never calibrated: cannot sound
  cfg.notes[2].airflowNominalPercent = 0;
  cfg.notes[2].airflowMinPercent = 0;
  cfg.notes[4].fingerPattern[1] = 7;     // not a fingering the controller can play

  CapabilitySnapshot s = snapshotOf(1, 1);
  assert(s.instrument.notes.size() == 3);
  assert(s.instrument.notes[0] == 60 && s.instrument.notes[1] == 61 &&
         s.instrument.notes[2] == 63);
  assert(!isPlayableNote(cfg, 2));
  assert(!isPlayableNote(cfg, 4));

  std::string j = GmbDescriptor::toJson(s);
  assert(jsonValue(j, "notes") == "{\"mode\":\"discrete\",\"list\":[60,61,63]}");
  // A note the physical instrument cannot play is never announced.
  assert(j.find("62") == std::string::npos || jsonValue(j, "notes").find("62") == std::string::npos);
}

void gmb_descriptor_channel_and_identity() {
  makeConfig(4, 60, 1);
  cfg.midiChannel = 5;              // 1-16 in the configuration
  strncpy(cfg.deviceName, "Atelier flute", sizeof(cfg.deviceName) - 1);
  std::string j = GmbDescriptor::toJson(snapshotOf(1, 1));
  assert(jsonValue(j, "channel") == "4");            // descriptor is 0-15
  assert(jsonValue(j, "name", jsonFind(j, "device")) == "\"Atelier flute\"");

  // Omni maps to channel 0: the channel GMB can always reach.
  cfg.midiChannel = 0;
  j = GmbDescriptor::toJson(snapshotOf(1, 1));
  assert(jsonValue(j, "channel") == "0");
}

void gmb_descriptor_embouchure_vocabulary() {
  struct Row { const char* emb; const char* program; const char* subtype; };
  const Row rows[] = {
    {"trav", "73", "\"flute\""},
    {"bec",  "74", "\"recorder\""},
    {"naf",  "77", "\"shakuhachi\""},
    {"end",  "77", "\"shakuhachi\""},
    {"oca",  "79", "\"ocarina\""},
  };
  for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
    makeConfig(4, 60, 1);
    memset(cfg.embouchure, 0, sizeof(cfg.embouchure));
    strncpy(cfg.embouchure, rows[i].emb, sizeof(cfg.embouchure) - 1);
    std::string j = GmbDescriptor::toJson(snapshotOf(1, 1));
    assert(jsonValue(j, "gm_program") == rows[i].program);
    assert(jsonValue(j, "subtype") == rows[i].subtype);
    assert(jsonValue(j, "type") == "\"pipe\"");        // always the same GMB type key
  }
}

void gmb_descriptor_timing() {
  makeConfig(4, 60, 1);
  cfg.servoToSolenoidDelayMs = 120;
  cfg.solenoidActivationTimeMs = 45;
  cfg.minNoteDurationMs = 25;
  cfg.minNoteIntervalForValveCloseMs = 60;
  std::string j = GmbDescriptor::toJson(snapshotOf(1, 1));

  // Slow silent finger positioning stays in `prepare`, so GMB can anticipate it.
  assert(jsonValue(j, "prepare") == "{\"base_ms\":120,\"max_ms\":120,\"silent\":true}");
  // Only the remaining valve latency is announced as `excite` - never the
  // preparation time folded in.
  assert(jsonValue(j, "excite") == "{\"latency_ms\":45}");
  assert(jsonValue(j, "min_note_ms") == "25");
  assert(jsonValue(j, "rearticulation_ms") == "60");
  // Nothing measures the acoustic decay: the field is absent, not zero.
  assert(!jsonHas(j, "release_ms"));
  assert(!jsonHas(j, "jitter_ms"));

  // An air mode with no valve has no measurable excitation latency: omitted.
  cfg.airMode = AIR_MODE_SERVO_ONLY;
  j = GmbDescriptor::toJson(snapshotOf(1, 1));
  assert(!jsonHas(j, "excite"));
  assert(!jsonHas(j, "rearticulation_ms"));
  assert(jsonValue(j, "prepare") == "{\"base_ms\":120,\"max_ms\":120,\"silent\":true}");
  // An omitted field is never announced as 0.
  assert(j.find("\"latency_ms\":0") == std::string::npos);
}

void gmb_descriptor_expression() {
  makeConfig(4, 60, 1);
  std::string j = GmbDescriptor::toJson(snapshotOf(1, 1));
  // CC1 vibrato, CC2 breath, CC7 volume, CC11 expression, CC73 attack shape.
  assert(jsonValue(j, "cc") == "[1,2,7,11,73]");
  assert(jsonValue(j, "pitch_bend") == "{\"supported\":false}");
  assert(jsonValue(j, "channel_aftertouch") == "false");
  assert(jsonValue(j, "poly_aftertouch") == "false");
  assert(jsonValue(j, "velocity") == "true");

  // A disabled breath controller is not announced.
  cfg.cc2Enabled = false;
  // No vibrato amplitude: CC1 changes nothing, so it is not announced either.
  cfg.vibratoMaxAmplitudeDeg = 0.0f;
  // Velocity has no influence at all on the sound.
  cfg.airVelocityResponse = 0;
  j = GmbDescriptor::toJson(snapshotOf(1, 1));
  assert(jsonValue(j, "cc") == "[7,11,73]");
  assert(jsonValue(j, "velocity") == "false");

  // CC74 steers the air jet, and only exists on a transverse embouchure whose
  // angle servo is enabled.
  makeConfig(4, 60, 1);
  cfg.angleServoEnabled = true;
  j = GmbDescriptor::toJson(snapshotOf(1, 1));
  assert(jsonValue(j, "cc") == "[1,2,7,11,73,74]");
  assert(jsonValue(j, "jet_angle_control") == "true");

  memset(cfg.embouchure, 0, sizeof(cfg.embouchure));
  strncpy(cfg.embouchure, "bec", sizeof(cfg.embouchure) - 1);
  j = GmbDescriptor::toJson(snapshotOf(1, 1));
  assert(jsonValue(j, "cc") == "[1,2,7,11,73]");     // no jet angle on a recorder
  assert(jsonValue(j, "jet_angle_control") == "false");
}

void gmb_descriptor_physical() {
  makeConfig(6, 60, 1);
  cfg.fingers[0].isThumbHole = true;
  cfg.notes[1].fingerPattern[3] = 2;    // one half-hole fingering
  std::string j = GmbDescriptor::toJson(snapshotOf(1, 1));
  assert(jsonValue(j, "family") == "\"winds\"");     // not a strings-shaped block
  assert(jsonValue(j, "embouchure") == "\"trav\"");
  assert(jsonValue(j, "finger_count") == "6");
  assert(jsonValue(j, "thumb_hole_count") == "1");
  assert(jsonValue(j, "half_holes") == "true");
  assert(jsonValue(j, "air_source") == "\"solenoid_valve\"");
  // No string-instrument field is forced into a wind instrument.
  assert(!jsonHas(j, "tuning"));
  assert(!jsonHas(j, "fret_count"));
  assert(!jsonHas(j, "string_count"));
}

void gmb_descriptor_unconfigured_instrument() {
  makeConfig(3, 60, 1);
  for (int i = 0; i < 3; i++) {
    cfg.notes[i].airflowMinPercent = 0;
    cfg.notes[i].airflowMaxPercent = 0;
    cfg.notes[i].airflowNominalPercent = 0;
  }
  CapabilitySnapshot s = snapshotOf(3, 1);
  assert(!s.instrument.configured);
  assert(!s.valid);

  std::string j = GmbDescriptor::toJson(s);
  assert(jsonWellFormed(j));
  assert(jsonValue(j, "configured") == "false");
  assert(jsonValue(j, "channel") == "0");
  // No fabricated fallback capabilities: GMB falls back to manual entry.
  assert(!jsonHas(j, "notes"));
  assert(!jsonHas(j, "expression"));
  assert(!jsonHas(j, "timing"));
  assert(!jsonHas(j, "gm_program"));

  // An invalid active configuration is never announced as configured either.
  makeConfig(3, 60, 1);
  CapabilitySnapshot invalid = snapshotOf(3, 1, false);
  assert(!invalid.instrument.configured);
  assert(jsonValue(GmbDescriptor::toJson(invalid), "configured") == "false");
}

void gmb_descriptor_is_ascii_and_escaped() {
  makeConfig(4, 60, 1);
  // A user-typed name with a quote, a backslash and non-ASCII text.
  const char* tricky = "Fl\xC3\xBBte \"6 trous\"\\";
  memset(cfg.deviceName, 0, sizeof(cfg.deviceName));
  strncpy(cfg.deviceName, tricky, sizeof(cfg.deviceName) - 1);
  std::string j = GmbDescriptor::toJson(snapshotOf(1, 1));
  assert(jsonWellFormed(j));
  for (size_t i = 0; i < j.size(); i++) {
    assert(((unsigned char)j[i] & 0x80) == 0);   // pure 7-bit ASCII
  }
  assert(j.find("\\u00fb") != std::string::npos); // û escaped
  assert(j.find("\\\"") != std::string::npos);
  assert(j.find("\\\\") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 4. Block 0x10 segmented transfer
// ---------------------------------------------------------------------------
void gmb_descriptor_transfer() {
  makeConfig(30, 48, 3);            // a long discrete list: several segments
  GmbSysExService svc;
  svc.setSnapshot(snapshotOf(9, 0x2222u));
  const std::string doc = svc.descriptorJson();
  const uint16_t total = GmbSysEx::chunkCount(doc.size());
  assert(total >= 3);               // first / middle / last really exist

  std::string rebuilt;
  uint32_t now = 100;
  for (uint16_t i = 0; i < total; i++) {
    std::vector<uint8_t> req = chunkRequest(i);
    std::vector<uint8_t> m = svc.handleMessage(req.data(), req.size(), now++);
    assert(m.size() >= 10);
    assert(m[0] == 0xF0 && m[1] == 0x7D && m[2] == 0x00);
    assert(m[3] == 0x10 && m[4] == 0x01);
    assert(m.back() == 0xF7);
    assert(payload7BitSafe(m));
    uint16_t gotTotal = (uint16_t)((m[5] & 0x7F) | ((m[6] & 0x7F) << 7));
    uint16_t gotIndex = (uint16_t)((m[7] & 0x7F) | ((m[8] & 0x7F) << 7));
    assert(gotTotal == total);
    assert(gotIndex == i);
    const size_t payload = m.size() - 10;
    assert(payload <= 200);         // spec cap, keeps the frame at 210 bytes
    if (i + 1 < total) assert(payload == 200);
    for (size_t k = 9; k + 1 < m.size(); k++) rebuilt.push_back((char)m[k]);
  }
  assert(rebuilt == doc);           // exact reconstruction
  assert(jsonWellFormed(rebuilt));

  // A duplicate request returns the identical segment.
  std::vector<uint8_t> req = chunkRequest(1);
  std::vector<uint8_t> a = svc.handleMessage(req.data(), req.size(), 500);
  std::vector<uint8_t> b = svc.handleMessage(req.data(), req.size(), 501);
  assert(!a.empty() && a == b);

  // An out-of-range index fails safely: no frame at all, so nothing can be
  // reassembled from a segment that does not exist.
  std::vector<uint8_t> bad = chunkRequest((uint16_t)(total + 5));
  assert(svc.handleMessage(bad.data(), bad.size(), 600).empty());
  std::vector<uint8_t> bad2 = chunkRequest(0x3FFF);
  assert(svc.handleMessage(bad2.data(), bad2.size(), 601).empty());
}

void gmb_descriptor_stable_during_transfer() {
  makeConfig(30, 48, 3);
  GmbSysExService svc;
  svc.setSnapshot(snapshotOf(1, 0x3333u));
  const std::string first = svc.descriptorJson();
  const uint16_t total = GmbSysEx::chunkCount(first.size());
  assert(total >= 2);

  std::vector<uint8_t> req0 = chunkRequest(0);
  std::vector<uint8_t> c0 = svc.handleMessage(req0.data(), req0.size(), 1000);
  assert(!c0.empty());

  // The user saves a different configuration mid-transfer.
  makeConfig(4, 60, 1);
  svc.setSnapshot(snapshotOf(2, 0x3333u));
  assert(svc.descriptorJson() != first);

  // The remaining segments still come from the document the transfer started on,
  // so the controller never reassembles two profile versions into one document.
  std::string rebuilt;
  for (size_t k = 9; k + 1 < c0.size(); k++) rebuilt.push_back((char)c0[k]);
  for (uint16_t i = 1; i < total; i++) {
    std::vector<uint8_t> req = chunkRequest(i);
    std::vector<uint8_t> m = svc.handleMessage(req.data(), req.size(), 1000 + i);
    assert(!m.empty());
    for (size_t k = 9; k + 1 < m.size(); k++) rebuilt.push_back((char)m[k]);
  }
  assert(rebuilt == first);

  // The next transfer picks up the new document, in full.
  const std::string second = svc.descriptorJson();
  const uint16_t total2 = GmbSysEx::chunkCount(second.size());
  std::string again;
  for (uint16_t i = 0; i < total2; i++) {
    std::vector<uint8_t> req = chunkRequest(i);
    std::vector<uint8_t> m = svc.handleMessage(req.data(), req.size(), 2000 + i);
    assert(!m.empty());
    for (size_t k = 9; k + 1 < m.size(); k++) again.push_back((char)m[k]);
  }
  assert(again == second);
  assert(again != first);

  // An abandoned transfer (the controller stopped mid-way) releases the pinned
  // document instead of serving a stale one forever.
  std::vector<uint8_t> c0b = svc.handleMessage(req0.data(), req0.size(), 3000);
  assert(!c0b.empty());
  makeConfig(20, 50, 2);
  svc.setSnapshot(snapshotOf(3, 0x3333u));
  const std::string third = svc.descriptorJson();
  std::vector<uint8_t> req1 = chunkRequest(1);
  std::vector<uint8_t> late = svc.handleMessage(req1.data(), req1.size(), 3000 + 6000);
  std::string lateText;
  for (size_t k = 9; k + 1 < late.size(); k++) lateText.push_back((char)late[k]);
  assert(third.compare(200, lateText.size(), lateText) == 0);
}

// ---------------------------------------------------------------------------
// 5. Revision management
// ---------------------------------------------------------------------------
void gmb_revision_lifecycle() {
  makeConfig(6, 60, 1);
  CapabilitySignature sig = computeSignature(snapshotOf(0, 1));

  // First boot: nothing persisted. Starts at 1 and must be written.
  RevisionTracker t;
  assert(t.begin(0, 0, sig));
  assert(t.revision() == 1);

  uint32_t storedRev = t.revision();
  uint32_t storedSig = t.signature();

  // A plain reboot with an unchanged configuration: no increment, no write.
  RevisionTracker rebooted;
  assert(!rebooted.begin(storedRev, storedSig, sig));
  assert(rebooted.revision() == storedRev);      // survives the reboot

  // A save that changes nothing announced (Wi-Fi credentials, UI preferences):
  // no increment, no notification.
  assert(!rebooted.onConfigurationActivated(sig, false));
  assert(rebooted.revision() == storedRev);
  assert(rebooted.changeFlags() == 0);

  // A real capability change: one increment and the matching flags.
  cfg.numNotes = 5;                              // one fingering removed
  CapabilitySignature next = computeSignature(snapshotOf(0, 1));
  assert(rebooted.onConfigurationActivated(next, false));
  assert(rebooted.revision() == storedRev + 1);
  assert((rebooted.changeFlags() & kInstrumentsChanged) != 0);
  assert((rebooted.changeFlags() & kIdentityChanged) == 0);
  assert((rebooted.changeFlags() & kTimingChanged) == 0);

  // A timing-only change reports TIMING_CHANGED alone.
  cfg.servoToSolenoidDelayMs = 150;
  CapabilitySignature timing = computeSignature(snapshotOf(0, 1));
  assert(rebooted.onConfigurationActivated(timing, false));
  assert(rebooted.changeFlags() == kTimingChanged);

  // An identity-only change reports IDENTITY_CHANGED alone.
  strncpy(cfg.deviceName, "Nouveau nom", sizeof(cfg.deviceName) - 1);
  CapabilitySignature ident = computeSignature(snapshotOf(0, 1));
  assert(rebooted.onConfigurationActivated(ident, false));
  assert(rebooted.changeFlags() == kIdentityChanged);

  // RESTART_REQUIRED rides along with whatever moved.
  cfg.numFingers = 5;
  CapabilitySignature restart = computeSignature(snapshotOf(0, 1));
  assert(rebooted.onConfigurationActivated(restart, true));
  assert((rebooted.changeFlags() & kRestartRequired) != 0);

  // The revision itself is not part of the signature: bumping it must not make
  // the next comparison see a change.
  CapabilitySnapshot a = snapshotOf(1, 1);
  CapabilitySnapshot b = snapshotOf(9999, 1);
  assert(computeSignature(a).all == computeSignature(b).all);
  // Neither is the instance id (it is not a capability).
  CapabilitySnapshot c = snapshotOf(1, 0xABCDEF01u);
  assert(computeSignature(a).all == computeSignature(c).all);
}

void gmb_revision_offline_change_detected_at_boot() {
  makeConfig(6, 60, 1);
  CapabilitySignature before = computeSignature(snapshotOf(0, 1));
  RevisionTracker t;
  t.begin(0, 0, before);
  const uint32_t stored = t.revision();

  // The configuration file is replaced while the firmware is not running (or a
  // restart-required change lands on the next boot). The counter must advance
  // exactly once, so a controller sees a new ETag.
  makeConfig(9, 60, 1);
  CapabilitySignature after = computeSignature(snapshotOf(0, 1));
  RevisionTracker rebooted;
  assert(rebooted.begin(stored, before.all, after));
  assert(rebooted.revision() == stored + 1);
  // And a second boot on that same configuration changes nothing further.
  RevisionTracker again;
  assert(!again.begin(rebooted.revision(), rebooted.signature(), after));
  assert(again.revision() == rebooted.revision());
}

// ---------------------------------------------------------------------------
// 6. Block 0x11 notification
// ---------------------------------------------------------------------------
void gmb_change_notification() {
  makeConfig(6, 60, 1);
  GmbSysExService svc;
  svc.setSnapshot(snapshotOf(1234, 0x4444u));

  std::vector<uint8_t> m = svc.notification(kInstrumentsChanged | kTimingChanged);
  assert(m.size() == 12);
  assert(m[0] == 0xF0 && m[1] == 0x7D && m[2] == 0x00);
  assert(m[3] == 0x11 && m[4] == 0x02);          // block 0x11, notification
  assert(gmbDecode32(&m[5]) == 1234u);           // the revision GMB will compare
  assert(m[10] == (kInstrumentsChanged | kTimingChanged));
  assert(m[11] == 0xF7);
  assert(payload7BitSafe(m));

  // Emitted only after activation: the bridge sends what the service holds, and
  // the service only holds an activated snapshot.
  GmbMidiBridge bridge;
  FakePort ble;
  bridge.begin(&svc);
  bridge.registerPort(&ble);

  svc.setSnapshot(snapshotOf(1235, 0x4444u));
  bridge.notifyCapabilitiesChanged(kInstrumentsChanged);
  assert(ble.sent.size() == 1);
  assert(gmbDecode32(&ble.sent[0][5]) == 1235u);

  // A disconnected transport is not written to.
  ble.sent.clear();
  ble.connected = false;
  bridge.notifyCapabilitiesChanged(kInstrumentsChanged);
  assert(ble.sent.empty());
}

// Mirrors what GmbRuntime::onConfigurationActivated() does, minus the ESP32
// glue (eFuse MAC + NVS): recompute, decide, persist, rebuild, notify.
struct FakeRuntime {
  GmbSysExService service;
  GmbMidiBridge bridge;
  RevisionTracker revision;
  uint32_t persistedRevision = 0;
  uint32_t persistedSignature = 0;
  int persistWrites = 0;

  void boot(uint32_t instanceId) {
    CapabilitySnapshot probe = snapshotOf(persistedRevision, instanceId);
    CapabilitySignature sig = computeSignature(probe);
    if (revision.begin(persistedRevision, persistedSignature, sig)) {
      persistedRevision = revision.revision();
      persistedSignature = revision.signature();
      persistWrites++;
    }
    probe.revision = revision.revision();
    service.setSnapshot(probe);
    bridge.begin(&service);
  }

  // Returns true when block 0x11 was emitted.
  bool activate(uint32_t instanceId) {
    CapabilitySnapshot next = snapshotOf(revision.revision(), instanceId);
    CapabilitySignature sig = computeSignature(next);
    if (!revision.onConfigurationActivated(sig, false)) {
      next.revision = revision.revision();
      service.setSnapshot(next);
      return false;
    }
    persistedRevision = revision.revision();
    persistedSignature = revision.signature();
    persistWrites++;
    next.revision = revision.revision();
    service.setSnapshot(next);
    bridge.notifyCapabilitiesChanged(revision.changeFlags());
    return true;
  }
};

void gmb_notification_only_on_a_real_activation() {
  makeConfig(6, 60, 1);
  FakeRuntime rt;
  FakePort port;
  rt.boot(0x8888u);
  rt.bridge.registerPort(&port);
  assert(rt.persistWrites == 1);          // first boot only
  const uint32_t first = rt.revision.revision();

  // A save that changes nothing announced: no revision, no flash write, and above
  // all no notification.
  assert(!rt.activate(0x8888u));
  assert(port.sent.empty());
  assert(rt.revision.revision() == first);
  assert(rt.persistWrites == 1);

  // A real change: revision advances, the descriptor is rebuilt, block 0x11 goes
  // out, and it carries the NEW revision - never the old one.
  const std::string before = rt.service.descriptorJson();
  cfg.numNotes = 4;
  assert(rt.activate(0x8888u));
  assert(rt.revision.revision() == first + 1);
  assert(rt.persistWrites == 2);
  assert(port.sent.size() == 1);
  assert(port.sent[0].size() == 12);
  assert(gmbDecode32(&port.sent[0][5]) == rt.revision.revision());
  assert((port.sent[0][10] & kInstrumentsChanged) != 0);
  assert(rt.service.descriptorJson() != before);

  // The descriptor is rebuilt BEFORE the notification is emitted, so a controller
  // that reacts immediately always finds the matching document.
  std::vector<uint8_t> hsReq = handshakeRequest();
  std::vector<uint8_t> hs = rt.service.handleMessage(hsReq.data(), hsReq.size(), 100);
  assert(gmbDecode32(&hs[17]) == rt.revision.revision());
  assert(decode21Le7(&hs[14]) == rt.service.descriptorJson().size());

  // A reboot on that configuration announces the same revision and writes nothing.
  FakeRuntime rebooted;
  rebooted.persistedRevision = rt.persistedRevision;
  rebooted.persistedSignature = rt.persistedSignature;
  rebooted.boot(0x8888u);
  assert(rebooted.persistWrites == 0);
  assert(rebooted.revision.revision() == rt.revision.revision());
}

// ---------------------------------------------------------------------------
// 7. Transport routing
// ---------------------------------------------------------------------------
void gmb_bridge_routes_back_to_origin() {
  makeConfig(6, 60, 1);
  GmbSysExService svc;
  svc.setSnapshot(snapshotOf(5, 0x5555u));

  GmbMidiBridge bridge;
  FakePort ble, rtp;
  ble.name = "ble";
  rtp.name = "rtpmidi";
  bridge.begin(&svc);
  bridge.registerPort(&ble);
  bridge.registerPort(&rtp);

  // Nothing is answered from the callback itself...
  std::vector<uint8_t> req = handshakeRequest();
  bridge.onSysEx(&rtp, req.data(), req.size());
  assert(ble.sent.empty() && rtp.sent.empty());

  // ...the reply is produced by the main loop, on the originating transport only.
  bridge.service(1000);
  assert(ble.sent.empty());
  assert(rtp.sent.size() == 1);
  assert(rtp.sent[0].size() == 24);

  // The other transport is served the same way from the same service.
  bridge.onSysEx(&ble, req.data(), req.size());
  bridge.service(1010);
  assert(ble.sent.size() == 1 && ble.sent[0].size() == 24);
  assert(ble.sent[0] == rtp.sent[0]);      // one protocol implementation, not two

  // Registering the same port twice does not double its replies.
  bridge.registerPort(&ble);
  ble.sent.clear();
  bridge.notifyCapabilitiesChanged(kInstrumentsChanged);
  assert(ble.sent.size() == 1);

  // A second request arriving before the loop runs is dropped, not queued: the
  // staging buffer is a single fixed-size slot.
  bridge.onSysEx(&ble, req.data(), req.size());
  bridge.onSysEx(&ble, req.data(), req.size());
  assert(bridge.overrunCount() == 1);
  ble.sent.clear();
  bridge.service(1020);
  assert(ble.sent.size() == 1);
  bridge.service(1030);
  assert(ble.sent.size() == 1);            // nothing left pending
}

// ---------------------------------------------------------------------------
// 8. Robustness
// ---------------------------------------------------------------------------
void gmb_malformed_input_is_ignored() {
  makeConfig(6, 60, 1);
  GmbSysExService svc;
  svc.setSnapshot(snapshotOf(1, 0x6666u));

  struct Case { const char* what; std::vector<uint8_t> bytes; };
  const Case cases[] = {
    {"empty",                {}},
    {"too short",            {0xF0, 0x7D, 0x00, 0xF7}},
    {"truncated handshake",  {0xF0, 0x7D, 0x00, 0x01, 0x00}},
    {"no F7",                {0xF0, 0x7D, 0x00, 0x01, 0x00, 0x00}},
    {"no F0",                {0x7D, 0x00, 0x01, 0x00, 0xF7, 0xF7}},
    {"wrong manufacturer",   {0xF0, 0x43, 0x00, 0x01, 0x00, 0xF7}},
    {"wrong GMB id",         {0xF0, 0x7D, 0x02, 0x01, 0x00, 0xF7}},
    {"unknown block",        {0xF0, 0x7D, 0x00, 0x42, 0x00, 0xF7}},
    {"legacy block 6",       {0xF0, 0x7D, 0x00, 0x06, 0x00, 0x00, 0xF7}},
    {"8-bit payload",        {0xF0, 0x7D, 0x00, 0x01, 0x80, 0xF7}},
    {"response echoed back", {0xF0, 0x7D, 0x00, 0x01, 0x01, 0xF7}},
    {"notification echoed",  {0xF0, 0x7D, 0x00, 0x11, 0x02, 0xF7}},
    {"chunk req too short",  {0xF0, 0x7D, 0x00, 0x10, 0x00, 0x00, 0xF7}},
    {"chunk req too long",   {0xF0, 0x7D, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0xF7}},
    {"handshake with junk",  {0xF0, 0x7D, 0x00, 0x01, 0x00, 0x01, 0xF7}},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    const std::vector<uint8_t>& b = cases[i].bytes;
    std::vector<uint8_t> out =
        svc.handleMessage(b.empty() ? 0 : b.data(), b.size(), (uint32_t)(2000 + i));
    if (!out.empty()) {
      std::cerr << "GMB: malformed input answered: " << cases[i].what << "\n";
      assert(false);
    }
  }
  // A null pointer never dereferences.
  assert(svc.handleMessage(0, 6, 3000).empty());

  // The bridge drops foreign / oversized SysEx before it costs a copy.
  GmbMidiBridge bridge;
  FakePort port;
  bridge.begin(&svc);
  bridge.registerPort(&port);
  std::vector<uint8_t> foreign(64, 0x00);
  foreign[0] = 0xF0; foreign[1] = 0x43; foreign.back() = 0xF7;
  bridge.onSysEx(&port, foreign.data(), foreign.size());
  bridge.service(4000);
  assert(port.sent.empty());
  assert(bridge.stagedCount() == 0);

  std::vector<uint8_t> huge(300, 0x00);
  huge[0] = 0xF0; huge[1] = 0x7D; huge[2] = 0x00; huge[3] = 0x10; huge[4] = 0x00;
  huge.back() = 0xF7;
  bridge.onSysEx(&port, huge.data(), huge.size());
  bridge.service(4010);
  assert(port.sent.empty());
  assert(bridge.stagedCount() == 0);
}

void gmb_flood_is_rate_limited() {
  makeConfig(30, 48, 3);
  GmbSysExService svc;
  svc.setSnapshot(snapshotOf(1, 0x7777u));
  std::vector<uint8_t> req = handshakeRequest();
  const uint16_t total = GmbSysEx::chunkCount(svc.descriptorJson().size());

  // A whole discovery (handshake + every descriptor segment, back to back in the
  // same millisecond) must never be rate-limited: that is the normal case.
  assert(!svc.handleMessage(req.data(), req.size(), 10000).empty());
  for (uint16_t i = 0; i < total; i++) {
    std::vector<uint8_t> c = chunkRequest(i);
    assert(!svc.handleMessage(c.data(), c.size(), 10000).empty());
  }

  // A sustained flood at the same instant is capped instead of forcing endless
  // responses and allocations on the main loop.
  int served = 0;
  for (int i = 0; i < 500; i++) {
    if (!svc.handleMessage(req.data(), req.size(), 10000).empty()) served++;
  }
  assert(served < 50);
  assert(svc.droppedRequests() >= 450);

  // Tokens refill with time: discovery still works after the flood.
  assert(!svc.handleMessage(req.data(), req.size(), 10200).empty());
}

}  // namespace

void gmb_run_all_tests() {
  gmb_instance_id_codec();
  gmb_instance_id_is_stable_and_unique();
  gmb_handshake_frame();
  gmb_descriptor_contiguous_range();
  gmb_descriptor_discrete_notes();
  gmb_descriptor_excludes_disabled_fingerings();
  gmb_descriptor_channel_and_identity();
  gmb_descriptor_embouchure_vocabulary();
  gmb_descriptor_timing();
  gmb_descriptor_expression();
  gmb_descriptor_physical();
  gmb_descriptor_unconfigured_instrument();
  gmb_descriptor_is_ascii_and_escaped();
  gmb_descriptor_transfer();
  gmb_descriptor_stable_during_transfer();
  gmb_revision_lifecycle();
  gmb_revision_offline_change_detected_at_boot();
  gmb_change_notification();
  gmb_notification_only_on_a_real_activation();
  gmb_bridge_routes_back_to_origin();
  gmb_malformed_input_is_ignored();
  gmb_flood_is_rate_limited();
}

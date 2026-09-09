// Host tool: prints General-Midi-Boop artefacts for a few configurations so an
// independent parser (tests/test_gmb_descriptor.py) can check that the descriptor
// really is valid JSON and that the handshake agrees with it.
//
// It lives outside tests/test_native/ on purpose: PlatformIO only collects
// directories named test_* as test suites, so this second main() never joins the
// native test binary.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Arduino.h"
#include "ConfigStorage.h"
#include "gmb/Capabilities.h"
#include "gmb/GmbDescriptor.h"
#include "gmb/GmbInstanceId.h"
#include "gmb/GmbSysEx.h"

using namespace gmb;

namespace {

void baseConfig(uint8_t count, uint8_t first, uint8_t step, const char* embouchure) {
  memset(&cfg, 0, sizeof(cfg));
  cfg.numFingers = 6;
  cfg.numNotes = count;
  cfg.airflowPcaChannel = 10;
  cfg.fingerAngleOpen = 30;
  cfg.halfHolePercent = 50;
  strncpy(cfg.embouchure, embouchure, sizeof(cfg.embouchure) - 1);
  for (int i = 0; i < 6; i++) {
    cfg.fingers[i].pcaChannel = (uint8_t)i;
    cfg.fingers[i].closedAngle = 90;
    cfg.fingers[i].direction = -1;
  }
  for (int i = 0; i < count; i++) {
    cfg.notes[i].midiNote = (uint8_t)(first + i * step);
    cfg.notes[i].airflowMinPercent = 10;
    cfg.notes[i].airflowMaxPercent = 60;
    cfg.notes[i].airflowNominalPercent = 30;
    cfg.notes[i].anglePercent = 50;
  }
  cfg.midiChannel = 0;
  cfg.servoToSolenoidDelayMs = 105;
  cfg.minNoteIntervalForValveCloseMs = 50;
  cfg.minNoteDurationMs = 10;
  cfg.servoAirflowOff = 20;
  cfg.servoAirflowMin = 60;
  cfg.servoAirflowMax = 100;
  cfg.servoAngleOff = 90;
  cfg.servoAngleMin = 60;
  cfg.servoAngleMax = 120;
  cfg.vibratoMaxAmplitudeDeg = 8.0f;
  cfg.cc2Enabled = true;
  cfg.solenoidActivationTimeMs = 50;
  cfg.airVelocityResponse = 60;
  cfg.airMode = AIR_MODE_SOLENOID_SERVO;
  cfg.valveType = 0;
  cfg.solenoidPin = SOLENOID_PIN;
  cfg.numPumps = 1;
  strncpy(cfg.deviceName, "ServoFlute", sizeof(cfg.deviceName) - 1);
}

void emit(const char* label, uint32_t revision, bool validated) {
  CapabilitySnapshot s = buildSnapshot(cfg, validated);
  s.revision = revision;
  s.identity.instanceId = instanceIdFromHardwareId(0x0000A4CF12345678ull);
  const std::string json = GmbDescriptor::toJson(s);
  const std::vector<uint8_t> hs = GmbSysEx::encodeHandshake(s, (uint32_t)json.size(), 0x03);

  printf("### %s\n", label);
  printf("DESCRIPTOR %s\n", json.c_str());
  printf("HANDSHAKE ");
  for (size_t i = 0; i < hs.size(); i++) printf("%02X", hs[i]);
  printf("\n");
  // Every segment of the block 0x10 transfer, so the reassembly is checked too.
  const uint16_t total = GmbSysEx::chunkCount(json.size());
  for (uint16_t i = 0; i < total; i++) {
    std::vector<uint8_t> c = GmbSysEx::encodeDescriptorChunk(json, i);
    printf("CHUNK ");
    for (size_t k = 0; k < c.size(); k++) printf("%02X", c[k]);
    printf("\n");
  }
}

}  // namespace

int main() {
  baseConfig(13, 72, 1, "trav");
  emit("contiguous", 1, true);

  baseConfig(14, 82, 1, "trav");
  // The default Irish-flute preset skips semitones: a discrete note list.
  const uint8_t preset[] = {82, 83, 84, 86, 88, 89, 91, 93, 95, 96, 98, 100, 101, 103};
  for (int i = 0; i < 14; i++) cfg.notes[i].midiNote = preset[i];
  cfg.fingers[0].isThumbHole = true;
  cfg.notes[3].fingerPattern[2] = 2;
  emit("discrete", 12345, true);

  baseConfig(20, 48, 3, "bec");
  cfg.midiChannel = 10;
  cfg.angleServoEnabled = true;    // ignored: not a transverse embouchure
  strncpy(cfg.deviceName, "Fl\xC3\xBBte \"test\"\\", sizeof(cfg.deviceName) - 1);
  emit("long-escaped", 0xFFFFFFFFu, true);

  baseConfig(4, 60, 1, "trav");
  for (int i = 0; i < 4; i++) {
    cfg.notes[i].airflowMinPercent = 0;
    cfg.notes[i].airflowMaxPercent = 0;
    cfg.notes[i].airflowNominalPercent = 0;
  }
  emit("unconfigured", 3, true);

  return 0;
}

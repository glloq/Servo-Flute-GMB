#include "Capabilities.h"

#include <string.h>

#include "../ConfigStorage.h"
#include "../settings.h"

namespace gmb {

namespace {

// GMB descriptor `device.model` (GMB docs/SYSEX_IDENTITY.md section 10).
const char* kModel = "Servo-Flute-GMB";

struct EmbouchureProfile {
  const char* key;       // cfg.embouchure value
  const char* name;      // instrument display name
  const char* subtype;   // InstrumentTypeConfig subtype key
  uint8_t gmProgram;     // General MIDI program for that subtype
};

// Mapping from the configured embouchure to the General-Midi-Boop vocabulary.
// `type` is always "pipe" (the InstrumentTypeConfig top-level key that owns GM
// programs 72-79); the subtype keys below are the existing ones from that same
// table - no new vocabulary is invented here.
const EmbouchureProfile kEmbouchures[] = {
  {"trav", "Transverse flute",     "flute",      73},  // flute traversiere
  {"bec",  "Recorder",             "recorder",   74},  // flute a bec / whistle
  {"naf",  "Native American flute", "shakuhachi", 77},  // no GM entry: nearest timbre
  {"end",  "End-blown flute",      "shakuhachi", 77},
  {"oca",  "Ocarina",              "ocarina",    79},
};

const EmbouchureProfile& embouchureProfile(const char* embouchure) {
  for (size_t i = 0; i < sizeof(kEmbouchures) / sizeof(kEmbouchures[0]); i++) {
    if (strncmp(embouchure, kEmbouchures[i].key, 5) == 0) return kEmbouchures[i];
  }
  return kEmbouchures[0];  // configuration default is "trav"
}

const char* airSourceLabel(const RuntimeConfig& c) {
  switch (c.airMode) {
    case AIR_MODE_SOLENOID_SERVO: return c.valveType == 1 ? "servo_valve" : "solenoid_valve";
    case AIR_MODE_SERVO_VALVE:    return "servo_valve";
    case AIR_MODE_SERVO_ONLY:     return "servo_only";
    case AIR_MODE_FAN_SERVO:      return "fan";
    case AIR_MODE_PUMP_VALVE:     return "pump";
    case AIR_MODE_PUMP_RESERVOIR: return "pump_reservoir";
    default:                      return "unknown";
  }
}

}  // namespace

bool isPlayableNote(const RuntimeConfig& config, uint8_t index) {
  if (index >= config.numNotes || index >= MAX_NOTES) return false;
  const NoteConfig& n = config.notes[index];
  if (n.midiNote > 127) return false;
  // A note whose airflow window is closed can never sound: setAirflowForNote()
  // would hold the airflow servo at the calibrated minimum, which is by definition
  // below the speaking threshold. Announcing it would promise a note the
  // instrument cannot play.
  if (n.airflowMaxPercent == 0) return false;
  if (n.airflowMinPercent > n.airflowMaxPercent) return false;
  if (n.airflowNominalPercent < n.airflowMinPercent) return false;
  if (n.airflowNominalPercent > n.airflowMaxPercent) return false;
  // A fingering value outside {closed, open, half} is not a fingering the
  // FingerController can execute.
  for (uint8_t f = 0; f < config.numFingers && f < MAX_FINGER_SERVOS; f++) {
    if (n.fingerPattern[f] > 2) return false;
  }
  return true;
}

CapabilitySnapshot buildSnapshot(const RuntimeConfig& config, bool configValidated) {
  CapabilitySnapshot snap;

  // ---- Identity -------------------------------------------------------------
  // Bounded reads: a fixed-size char array that lost its terminator must not walk
  // off the end of the configuration struct and into the descriptor.
  snap.identity.deviceName =
      std::string(config.deviceName, strnlen(config.deviceName, sizeof(config.deviceName)));
  snap.identity.model = kModel;
  snap.identity.firmware[0] = FIRMWARE_VERSION_MAJOR;
  snap.identity.firmware[1] = FIRMWARE_VERSION_MINOR;
  snap.identity.firmware[2] = FIRMWARE_VERSION_PATCH;
  // instanceId is injected by the platform layer (ESP32 eFuse MAC); it is not a
  // configuration value and must never change when the profile changes.

  InstrumentCapabilities& inst = snap.instrument;

  // ---- Identification -------------------------------------------------------
  // cfg.midiChannel is 0 = omni, 1-16 = a specific channel. The descriptor channel
  // is 0-15. In omni the instrument accepts everything, so channel 0 is the
  // channel GMB can always reach.
  inst.channel = (config.midiChannel == 0) ? 0 : (uint8_t)(config.midiChannel - 1);
  if (inst.channel > 15) inst.channel = 15;

  const EmbouchureProfile& emb = embouchureProfile(config.embouchure);
  inst.name = emb.name;
  inst.type = "pipe";
  inst.subtype = emb.subtype;
  inst.gmProgram = emb.gmProgram;

  // ---- Playable notes (section 5.3) ----------------------------------------
  // Built from the ACTIVE fingering table, never from a hard-coded range.
  bool seen[128];
  for (int i = 0; i < 128; i++) seen[i] = false;
  for (uint8_t i = 0; i < config.numNotes && i < MAX_NOTES; i++) {
    if (!isPlayableNote(config, i)) continue;
    uint8_t note = config.notes[i].midiNote;
    if (seen[note]) continue;   // a duplicate is announced once
    seen[note] = true;
  }
  for (int n = 0; n < 128; n++) {
    if (seen[n]) inst.notes.push_back((uint8_t)n);
  }

  if (!inst.notes.empty()) {
    inst.noteMin = inst.notes.front();
    inst.noteMax = inst.notes.back();
    // Continuous only when every semitone between min and max is playable.
    bool contiguous = ((size_t)(inst.noteMax - inst.noteMin) + 1u) == inst.notes.size();
    inst.noteMode = contiguous ? kNoteRange : kNoteDiscrete;
  }

  // ---- Half-hole / thumb-hole facts, read off the announced fingerings -------
  for (uint8_t f = 0; f < config.numFingers && f < MAX_FINGER_SERVOS; f++) {
    if (config.fingers[f].isThumbHole) inst.physical.thumbHoleCount++;
  }
  for (uint8_t i = 0; i < config.numNotes && i < MAX_NOTES; i++) {
    if (!isPlayableNote(config, i)) continue;
    for (uint8_t f = 0; f < config.numFingers && f < MAX_FINGER_SERVOS; f++) {
      if (config.notes[i].fingerPattern[f] == 2) { inst.physical.halfHoles = true; break; }
    }
    if (inst.physical.halfHoles) break;
  }

  // ---- configured (section 5.1) --------------------------------------------
  // Only true when the active configuration really can play: it validated, it has
  // fingers, it has at least one playable fingering, and the airflow servo has a
  // usable travel. Otherwise the instrument entry is announced as unconfigured and
  // GMB keeps manual entry instead of importing an empty capability set.
  inst.configured = configValidated &&
                    config.numFingers >= 1 &&
                    !inst.notes.empty() &&
                    config.servoAirflowMin < config.servoAirflowMax;
  snap.valid = inst.configured;

  // ---- Polyphony (section 5.5) ---------------------------------------------
  // The NoteSequencer owns exactly one note at a time (a new NOTE_ON replaces the
  // current one), and a single flute body has one air column: strictly monophonic.
  inst.polyphony = 1;

  // ---- Timing (section 5.6) ------------------------------------------------
  // prepare = silent mechanical finger positioning, before any air is admitted.
  // The sequencer holds the valve closed for servoToSolenoidDelayMs while the
  // finger servos travel, so GMB can anticipate the whole gesture during playback.
  // It is a fixed window, not a function of the interval, so base and max are equal
  // and per_semitone_ms is not announced.
  inst.timing.hasPrepare = true;
  inst.timing.prepareBaseMs = config.servoToSolenoidDelayMs;
  inst.timing.prepareMaxMs = config.servoToSolenoidDelayMs;

  // excite = what remains between opening the air path and the note speaking. Only
  // the solenoid valve exposes a configured figure: solenoidActivationTimeMs is the
  // full-power drive window that bounds its mechanical opening time. For a servo
  // valve, a fan or a direct pump the firmware has no measured figure, so the field
  // is omitted rather than invented (an unknown is never announced as 0).
  if (configurationUsesSolenoidValve(config)) {
    inst.timing.hasExciteLatency = true;
    inst.timing.exciteLatencyMs = config.solenoidActivationTimeMs;
  }

  inst.timing.hasMinNote = true;
  inst.timing.minNoteMs = config.minNoteDurationMs;

  // Two notes closer together than minNoteIntervalForValveCloseMs are slurred: the
  // sequencer keeps the valve open and no new attack is produced. That interval is
  // therefore the shortest gap that yields a real re-articulation. Modes with no
  // physical valve never close between notes, so no figure is announced.
  if (modeUsesPhysicalValve(config.airMode)) {
    inst.timing.hasRearticulation = true;
    inst.timing.rearticulationMs = config.minNoteIntervalForValveCloseMs;
  }
  // release_ms is not announced: the firmware has no measured acoustic decay and
  // inventing one would be worse than leaving GMB to ask the user.

  // ---- Expression (section 5.7) --------------------------------------------
  ExpressionModel& expr = inst.expression;
  // CC1 modulation drives the vibrato depth; with a zero amplitude the controller
  // is received but changes nothing, so it is not announced.
  if (config.vibratoMaxAmplitudeDeg > 0.0f) expr.cc.push_back(MIDI_CC_MODULATION);
  // CC2 breath is only consumed when the breath controller is enabled.
  if (config.cc2Enabled) expr.cc.push_back(MIDI_CC_BREATH);
  // CC7 volume and CC11 expression always scale the airflow window.
  expr.cc.push_back(MIDI_CC_VOLUME);
  expr.cc.push_back(MIDI_CC_EXPRESSION);
  // CC73 selects the airflow attack shape (stable / accent / crescendo).
  expr.cc.push_back(MIDI_CC_ATTACK_TIME);
  // CC74 steers the air jet and only exists on a transverse embouchure whose angle
  // servo is enabled.
  bool jetAngle = (strncmp(config.embouchure, "trav", 5) == 0) && config.angleServoEnabled;
  if (jetAngle) expr.cc.push_back(MIDI_CC_BRIGHTNESS);
  inst.physical.jetAngleControl = jetAngle;

  // Velocity shapes the airflow only when the velocity response is non-zero;
  // at 0 every note is played at its calibrated nominal airflow.
  expr.velocity = config.airVelocityResponse > 0;
  // Not implemented anywhere in the MIDI paths (BLE / rtpMIDI / DIN): announced as
  // explicitly unsupported rather than omitted, because "not supported" is a fact
  // the firmware knows, unlike an unmeasured latency.
  expr.pitchBend = false;
  expr.channelAftertouch = false;
  expr.polyAftertouch = false;

  // ---- Physical (section 5.9) ----------------------------------------------
  inst.physical.family = "winds";
  inst.physical.embouchure =
      std::string(config.embouchure, strnlen(config.embouchure, sizeof(config.embouchure)));
  inst.physical.fingerCount = config.numFingers;
  inst.physical.airSource = airSourceLabel(config);

  return snap;
}

}  // namespace gmb

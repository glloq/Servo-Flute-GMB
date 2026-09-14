/***********************************************************************************************
 * Capabilities - immutable GMB capability snapshot built from the ACTIVE RuntimeConfig.
 *
 * The Servo-Flute configuration (RuntimeConfig / ConfigStorage) is the single
 * source of truth. Nothing here is hard-coded that the configuration already
 * knows: the playable notes, the MIDI channel, the announced control changes, the
 * timing model and the physical description are all derived from the validated
 * active configuration.
 *
 * A snapshot is immutable once built, so every SysEx response and the HTTP
 * descriptor are served from one consistent version of the profile even if the
 * user saves a new configuration in the middle of a transfer.
 *
 * Pure core: no Arduino / ESP-IDF dependency beyond RuntimeConfig itself, so it is
 * unit tested on the host.
 ***********************************************************************************************/
#ifndef GMB_CAPABILITIES_H
#define GMB_CAPABILITIES_H

#include <stdint.h>

#include <string>
#include <vector>

struct RuntimeConfig;

namespace gmb {

// notes.mode of the descriptor (GMB docs/SYSEX_IDENTITY.md section 5.3).
enum NoteMode : uint8_t { kNoteRange = 0, kNoteDiscrete = 1 };

struct DeviceIdentity {
  uint32_t instanceId = 0;     // stable per physical board, never 0
  std::string deviceName;      // descriptor device.name  (cfg.deviceName)
  std::string model;           // descriptor device.model ("Servo-Flute-GMB")
  uint8_t firmware[3] = {0, 0, 0};
};

// Two-phase timing model (section 5.6). Every field is optional: an unknown value
// is omitted from the descriptor, never announced as 0.
struct TimingModel {
  bool hasPrepare = false;
  uint16_t prepareBaseMs = 0;
  uint16_t prepareMaxMs = 0;
  bool hasExciteLatency = false;
  uint16_t exciteLatencyMs = 0;
  bool hasMinNote = false;
  uint16_t minNoteMs = 0;
  bool hasRearticulation = false;
  uint16_t rearticulationMs = 0;
};

// Announced MIDI expression (section 5.7). Only features the firmware really
// implements AND the active configuration really enables are announced.
struct ExpressionModel {
  std::vector<uint8_t> cc;          // ascending, deduplicated
  bool pitchBend = false;
  uint8_t pitchBendRangeSemitones = 0;
  bool channelAftertouch = false;
  bool polyAftertouch = false;
  bool velocity = false;
};

// Wind-family `physical` block (section 5.9). Unknown keys are ignored by GMB, so
// flute-specific extras are safe; generic keys keep their generic meaning.
struct PhysicalModel {
  std::string family;          // "winds"
  std::string embouchure;      // trav | bec | naf | end | oca (configuration value)
  uint8_t fingerCount = 0;
  uint8_t thumbHoleCount = 0;
  bool halfHoles = false;      // at least one announced fingering uses a half hole
  std::string airSource;       // solenoid_valve | servo_valve | servo_only | fan | pump | pump_reservoir
  bool jetAngleControl = false;  // transverse embouchure with the angle servo enabled
};

struct InstrumentCapabilities {
  uint8_t channel = 0;            // descriptor channel 0-15
  bool configured = false;        // active configuration is sufficient to play
  std::string name;
  uint8_t gmProgram = 73;
  std::string type;               // InstrumentTypeConfig key, e.g. "pipe"
  std::string subtype;            // e.g. "flute"
  uint8_t noteMode = kNoteRange;
  uint8_t noteMin = 0;
  uint8_t noteMax = 0;
  std::vector<uint8_t> notes;     // every playable MIDI note, ascending, no duplicates
  uint8_t polyphony = 1;
  TimingModel timing;
  ExpressionModel expression;
  PhysicalModel physical;
};

struct CapabilitySnapshot {
  uint32_t revision = 0;
  DeviceIdentity identity;
  InstrumentCapabilities instrument;
  // True when the snapshot describes an instrument that can actually play. Mirrors
  // instrument.configured; kept separate so a caller can reason about the snapshot
  // without reaching into the instrument entry.
  bool valid = false;
};

// Build a snapshot from the active configuration.
// `configValidated` is the result of validateAndNormalizeConfig() on that very
// configuration: a configuration that does not validate is never announced as
// `configured`, so GMB falls back to manual entry instead of importing garbage.
CapabilitySnapshot buildSnapshot(const RuntimeConfig& config, bool configValidated);

// True when the note at `index` of `config` describes a fingering the instrument
// can actually play. Exposed for tests and for the descriptor/diagnostics paths.
bool isPlayableNote(const RuntimeConfig& config, uint8_t index);

// Acoustic excitation latency in milliseconds, or 0 when it is NOT KNOWN.
//
// `timing.excite.latency_ms` (GMB section 5.6) is the non-maskable delay between
// the MIDI order and the moment the note is actually AUDIBLE. General-Midi-Boop
// uses it to line several instruments up on the same beat, so a wrong figure is
// worse than no figure at all: an instrument that claims 0 is scheduled as if it
// spoke instantly.
//
// No configuration value measures that delay. In particular
// `solenoidActivationTimeMs` does NOT: it is the full-power drive window of the
// solenoid before the PWM drops to its holding level (AirflowController::update),
// i.e. an electrical parameter of the valve coil, unrelated to when the air
// column starts to speak.
//
// This is the single seam a real figure goes through. A future measurement -
// naturally an onset detection on the existing microphone path (AudioAnalyzer +
// AutoCalibrator), persisted as a `measuredExciteLatencyMs` configuration field -
// only has to be returned here: the snapshot, the descriptor, the revision
// signature and the block 0x11 notification already carry the value end to end.
// Until then the honest answer is 0 = unknown, and the descriptor omits the
// field per the GMB rule "an absent field means unknown".
uint16_t measuredExciteLatencyMs(const RuntimeConfig& config);

}  // namespace gmb

#endif

#include "GmbRevision.h"

#include "GmbInstanceId.h"
#include "GmbSysEx.h"

namespace gmb {

namespace {

// Small FNV-1a accumulator so a signature can be fed field by field without
// building an intermediate buffer.
struct Hasher {
  uint32_t h;
  Hasher() : h(2166136261u) {}
  void byte(uint8_t v) { h ^= (uint32_t)v; h *= 16777619u; }
  void u16(uint16_t v) { byte((uint8_t)(v & 0xFF)); byte((uint8_t)(v >> 8)); }
  void u32(uint32_t v) { u16((uint16_t)(v & 0xFFFF)); u16((uint16_t)(v >> 16)); }
  void boolean(bool v) { byte(v ? 1 : 0); }
  void text(const std::string& s) {
    for (size_t i = 0; i < s.size(); i++) byte((uint8_t)s[i]);
    byte(0);   // terminator so "ab"+"c" and "a"+"bc" differ
  }
};

}  // namespace

CapabilitySignature computeSignature(const CapabilitySnapshot& s) {
  const InstrumentCapabilities& inst = s.instrument;

  Hasher identity;
  identity.text(s.identity.deviceName);
  identity.text(s.identity.model);
  identity.byte(s.identity.firmware[0]);
  identity.byte(s.identity.firmware[1]);
  identity.byte(s.identity.firmware[2]);
  identity.byte(inst.channel);
  identity.boolean(inst.configured);
  identity.text(inst.name);
  identity.byte(inst.gmProgram);
  identity.text(inst.type);
  identity.text(inst.subtype);

  Hasher instruments;
  instruments.byte(inst.noteMode);
  instruments.byte(inst.noteMin);
  instruments.byte(inst.noteMax);
  instruments.u16((uint16_t)inst.notes.size());
  for (size_t i = 0; i < inst.notes.size(); i++) instruments.byte(inst.notes[i]);
  instruments.byte(inst.polyphony);
  instruments.u16((uint16_t)inst.expression.cc.size());
  for (size_t i = 0; i < inst.expression.cc.size(); i++) instruments.byte(inst.expression.cc[i]);
  instruments.boolean(inst.expression.pitchBend);
  instruments.byte(inst.expression.pitchBendRangeSemitones);
  instruments.boolean(inst.expression.channelAftertouch);
  instruments.boolean(inst.expression.polyAftertouch);
  instruments.boolean(inst.expression.velocity);
  instruments.text(inst.physical.family);
  instruments.text(inst.physical.embouchure);
  instruments.byte(inst.physical.fingerCount);
  instruments.byte(inst.physical.thumbHoleCount);
  instruments.boolean(inst.physical.halfHoles);
  instruments.text(inst.physical.airSource);
  instruments.boolean(inst.physical.jetAngleControl);

  Hasher timing;
  timing.boolean(inst.timing.hasPrepare);
  timing.u16(inst.timing.prepareBaseMs);
  timing.u16(inst.timing.prepareMaxMs);
  timing.boolean(inst.timing.hasExciteLatency);
  timing.u16(inst.timing.exciteLatencyMs);
  timing.boolean(inst.timing.hasMinNote);
  timing.u16(inst.timing.minNoteMs);
  timing.boolean(inst.timing.hasRearticulation);
  timing.u16(inst.timing.rearticulationMs);

  CapabilitySignature sig;
  sig.identity = identity.h;
  sig.instruments = instruments.h;
  sig.timing = timing.h;
  Hasher all;
  all.u32(sig.identity);
  all.u32(sig.instruments);
  all.u32(sig.timing);
  sig.all = all.h;
  return sig;
}

RevisionTracker::RevisionTracker() : _revision(0), _changeFlags(0), _seeded(false) {}

bool RevisionTracker::begin(uint32_t storedRevision, uint32_t storedSignature,
                            const CapabilitySignature& current) {
  _signature = current;
  _seeded = true;
  _changeFlags = 0;

  if (storedRevision == 0) {
    // Nothing persisted yet (first boot after flashing, or a wiped NVS).
    // The revision starts at 1; 0 would read as "never announced".
    _revision = 1;
    return true;
  }
  _revision = storedRevision;
  if (storedSignature == current.all) {
    // Same configuration as when the counter was last written: a reboot alone
    // must not move the revision, and nothing is written to flash.
    return false;
  }
  // The announced capabilities differ from what was persisted with this counter.
  // That is a real change (a config file replaced offline, a firmware upgrade
  // that changes what is announced), so the counter advances once.
  _revision++;
  _changeFlags = kIdentityChanged | kInstrumentsChanged | kTimingChanged;
  return true;
}

bool RevisionTracker::onConfigurationActivated(const CapabilitySignature& next,
                                               bool restartRequired) {
  if (!_seeded) {
    // Defensive: activation before begin() behaves like a first seed.
    return begin(0, 0, next);
  }
  uint8_t flags = 0;
  if (next.identity != _signature.identity) flags |= kIdentityChanged;
  if (next.instruments != _signature.instruments) flags |= kInstrumentsChanged;
  if (next.timing != _signature.timing) flags |= kTimingChanged;

  if (flags == 0) {
    // A save that changes nothing GMB is told about (Wi-Fi credentials, UI
    // preferences, a re-save of identical values): no increment, no flash write,
    // no notification.
    _changeFlags = 0;
    return false;
  }
  if (restartRequired) flags |= kRestartRequired;

  _signature = next;
  _revision++;
  _changeFlags = flags;
  return true;
}

}  // namespace gmb

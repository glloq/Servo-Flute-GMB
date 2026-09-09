/***********************************************************************************************
 * GmbRevision - persistent capability revision counter.
 *
 * `revision` is the ETag General-Midi-Boop uses to decide whether it needs to
 * re-download the descriptor. The contract (GMB docs/SYSEX_IDENTITY.md sections
 * 2 and 4) is:
 *
 *     configuration change -> validated -> committed -> active
 *         -> revision++ -> descriptor rebuilt -> block 0x11 emitted
 *
 * and, just as importantly, a plain reboot must NOT increment it.
 *
 * The counter is driven by a signature of the capability-relevant part of the
 * ACTIVE configuration rather than by "something was saved". That gives the whole
 * contract for free: a reboot recomputes the same signature and changes nothing; a
 * no-op save changes nothing; saving Wi-Fi credentials (not part of the announced
 * capabilities) changes nothing; and flash is only written on a real change.
 *
 * Three sub-signatures let the block 0x11 change flags say WHAT moved
 * (identity / instruments / timing) without a second source of truth.
 *
 * Pure core: persistence is the caller's job (NVS on the ESP32), so the whole
 * decision logic is unit tested on the host.
 ***********************************************************************************************/
#ifndef GMB_REVISION_H
#define GMB_REVISION_H

#include <stdint.h>

#include "Capabilities.h"

namespace gmb {

struct CapabilitySignature {
  uint32_t identity = 0;      // device name, model, firmware, MIDI channel, GM identity
  uint32_t instruments = 0;   // playable notes, polyphony, expression, physical
  uint32_t timing = 0;        // the two-phase timing model
  uint32_t all = 0;           // combination of the three, the persisted value
};

// Signature of the capability-relevant fields of a snapshot. The instance id and
// the revision itself are deliberately excluded: they are not capabilities, and
// including the revision would make the signature change every time it is bumped.
CapabilitySignature computeSignature(const CapabilitySnapshot& snapshot);

class RevisionTracker {
public:
  RevisionTracker();

  // Seed from persisted state at boot. `storedRevision` == 0 means "nothing
  // persisted yet". Returns true when the caller must persist the new state,
  // which happens on a first boot (nothing stored) or when the configuration
  // changed while the firmware was not running (a config file edited offline, a
  // firmware upgrade that changes what is announced). A plain reboot with an
  // unchanged configuration returns false and writes nothing.
  bool begin(uint32_t storedRevision, uint32_t storedSignature,
             const CapabilitySignature& current);

  // Report that a new configuration has been validated, committed and activated.
  // Returns true when the revision actually moved, i.e. when the announced
  // capabilities changed - that is the only case in which the caller persists the
  // counter, rebuilds the descriptor and emits block 0x11.
  bool onConfigurationActivated(const CapabilitySignature& next, bool restartRequired);

  uint32_t revision() const { return _revision; }
  uint32_t signature() const { return _signature.all; }
  // Change flags describing the most recent increment (block 0x11 payload).
  uint8_t changeFlags() const { return _changeFlags; }

private:
  uint32_t _revision;
  CapabilitySignature _signature;
  uint8_t _changeFlags;
  bool _seeded;
};

}  // namespace gmb

#endif

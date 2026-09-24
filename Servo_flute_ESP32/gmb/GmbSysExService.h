/***********************************************************************************************
 * GmbSysExService - transport-independent GMB SysEx endpoint.
 *
 * Holds one immutable capability snapshot plus the JSON descriptor built from it,
 * and answers incoming requests from that pair. The descriptor is built ONCE per
 * configuration activation and cached, so answering a block 0x10 segment costs a
 * substring copy, never a JSON render.
 *
 * A transfer in flight is pinned to the descriptor it started on: if the user
 * saves a new configuration while General-Midi-Boop is fetching, EVERY remaining
 * segment - a retry of segment 0 included - still comes from the document the
 * transfer began with, and the block 0x11 notification tells GMB to restart with
 * the new revision. The pin is chosen once, when a transfer starts, and released
 * only when the document has been delivered in full, when the controller
 * abandons the transfer (idle timeout), or when a handshake announces a document
 * the pinned one no longer matches.
 *
 * Every MIDI transport that can send SysEx both ways hands complete messages here
 * and writes back whatever bytes are returned; no protocol logic is duplicated
 * per transport.
 *
 * The descriptor is DISCOVERY METADATA, never part of playing a note. Nothing on
 * the actuator path (NoteSequencer / AirflowController / FingerController) reads
 * it, and a rebuild that cannot complete degrades discovery only: the previously
 * published document stays in place and the instrument goes on playing. See
 * setSnapshot() for what that costs and why it is built that way.
 ***********************************************************************************************/
#ifndef GMB_SYSEX_SERVICE_H
#define GMB_SYSEX_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "Capabilities.h"
#include "GmbDescriptor.h"
#include "GmbSysEx.h"

namespace gmb {

class GmbSysExService {
public:
  GmbSysExService();

  // Replace the active snapshot and rebuild the cached descriptor. Called only
  // after a configuration has been validated, committed and activated.
  //
  // ALL OR NOTHING: the snapshot and the descriptor are the pair the controller
  // reads (the handshake announces the snapshot's revision and the descriptor's
  // size, block 0x10 serves the descriptor), so they are published together or
  // not at all. A rebuild that cannot complete leaves BOTH at their previous,
  // matching values and counts one descriptorRebuildFailures().
  void setSnapshot(const CapabilitySnapshot& snapshot);
  const CapabilitySnapshot& snapshot() const { return _snapshot; }

  // The cached descriptor for the CURRENT snapshot, served over block 0x10 and
  // over GET /gmb/descriptor.json. Both read this same string, so they can never
  // diverge.
  //
  // _descriptor is NEVER null, so neither of these can dereference nothing: the
  // constructor publishes the "instrument present, not configured" document of
  // section 5.1 before anything else can run, and setSnapshot() only ever
  // assigns a pointer it has already checked. A constructor that could not build
  // that first document throws instead of leaving a half-built service behind,
  // so there is no state in which a GmbSysExService exists without a document.
  const std::string& descriptorJson() const { return *_descriptor; }
  uint32_t descriptorSize() const { return (uint32_t)_descriptor->size(); }

  // Handshake flags: bit 0 = an HTTP descriptor endpoint is reachable, bit 1 =
  // block 0x11 change notifications are emitted. Bit 0 follows the web server,
  // which only runs in Wi-Fi mode, so the flag is never announced when there is
  // no HTTP route to fetch.
  void setHttpDescriptorAvailable(bool available);
  uint8_t handshakeFlags() const { return _handshakeFlags; }

  // Handle one complete incoming SysEx message. Returns the bytes to send back,
  // or an empty vector when nothing should be sent (malformed frame, unknown
  // block, out-of-range segment, rate limited).
  std::vector<uint8_t> handleMessage(const uint8_t* data, size_t len, uint32_t nowMs);

  // Block 0x11 notification for the current revision.
  std::vector<uint8_t> notification(uint8_t changeFlags) const {
    return GmbSysEx::encodeChangeNotification(_snapshot.revision, changeFlags);
  }

  // Diagnostics.
  uint32_t handledRequests() const { return _handled; }
  uint32_t droppedRequests() const { return _dropped; }
  // Rebuilds that could not complete and therefore published nothing. Non-zero
  // means the served descriptor is OLDER than the active configuration: the
  // instrument plays the new configuration, General-Midi-Boop is still being
  // told about the previous one. Degraded discovery, never a degraded note.
  uint32_t descriptorRebuildFailures() const { return _rebuildFailures; }
  // True while a block 0x10 transfer is pinned to a document. Diagnostics and
  // tests only; the protocol never exposes it.
  bool transferInFlight() const { return (bool)_serving; }

private:
  CapabilitySnapshot _snapshot;
  // Scratch copy of the snapshot being published, kept from one activation to
  // the next ON PURPOSE. setSnapshot() copies into it and only then swaps it
  // with _snapshot, which is how the publication can be all-or-nothing without
  // paying for it: after the swap this member owns the buffers of the PREVIOUS
  // snapshot, so the next copy into it reuses those buffers instead of
  // allocating new ones. Costs one CapabilitySnapshot of permanent RAM (~0.4 kB)
  // and saves every allocation a staging copy would otherwise repeat per
  // activation. Never read: its contents between two calls are of no interest.
  CapabilitySnapshot _staged;
  // Shared ownership so a transfer can pin the document it started on without
  // copying it: a rebuild simply publishes a new string and the pinned one is
  // released when the transfer ends.
  std::shared_ptr<const std::string> _descriptor;
  std::shared_ptr<const std::string> _serving;   // pinned for the transfer in flight
  uint32_t _servingLastMs;
  // Segments delivered since this transfer started, retries included. A document
  // is only considered fully transferred once its last segment has gone out AND
  // at least as many segments as it has were delivered, so a controller that
  // fetches out of order cannot end the transfer on its first request.
  uint32_t _servingDelivered;

  uint8_t _handshakeFlags;
  uint32_t _handled;
  uint32_t _dropped;
  uint32_t _rebuildFailures;

  // A transfer is considered abandoned after this long without a segment request,
  // which releases the pinned document.
  static constexpr uint32_t kTransferIdleMs = 5000;

  // Token bucket: allows the discovery burst (handshake + every segment back to
  // back) but caps a sustained flood, so repeated SysEx traffic can never keep the
  // main loop building responses instead of servicing notes.
  static constexpr int kMaxTokens = 24;
  static constexpr uint32_t kRefillMs = 5;   // +1 token every 5 ms
  int _tokens;
  uint32_t _lastRefillMs;
  bool allow(uint32_t nowMs);

  // Release the pin of a transfer the controller stopped requesting segments for.
  void expireStaleTransfer(uint32_t nowMs);
  void endTransfer();
  // Answer one block 0x10 segment request, starting / advancing / ending the
  // transfer it belongs to.
  std::vector<uint8_t> serveDescriptorChunk(uint16_t index, uint32_t nowMs);
};

}  // namespace gmb

#endif

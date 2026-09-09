/***********************************************************************************************
 * GmbSysExService - transport-independent GMB SysEx endpoint.
 *
 * Holds one immutable capability snapshot plus the JSON descriptor built from it,
 * and answers incoming requests from that pair. The descriptor is built ONCE per
 * configuration activation and cached, so answering a block 0x10 segment costs a
 * substring copy, never a JSON render.
 *
 * A transfer in flight is pinned to the descriptor it started on: if the user
 * saves a new configuration while General-Midi-Boop is fetching, the remaining
 * segments still come from the document the transfer began with, and the block
 * 0x11 notification tells GMB to restart with the new revision.
 *
 * Every MIDI transport that can send SysEx both ways hands complete messages here
 * and writes back whatever bytes are returned; no protocol logic is duplicated
 * per transport.
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
  void setSnapshot(const CapabilitySnapshot& snapshot);
  const CapabilitySnapshot& snapshot() const { return _snapshot; }

  // The cached descriptor for the CURRENT snapshot, served over block 0x10 and
  // over GET /gmb/descriptor.json. Both read this same string, so they can never
  // diverge.
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

private:
  CapabilitySnapshot _snapshot;
  // Shared ownership so a transfer can pin the document it started on without
  // copying it: a rebuild simply publishes a new string and the pinned one is
  // released when the transfer ends.
  std::shared_ptr<const std::string> _descriptor;
  std::shared_ptr<const std::string> _serving;   // pinned for the transfer in flight
  uint32_t _servingLastMs;

  uint8_t _handshakeFlags;
  uint32_t _handled;
  uint32_t _dropped;

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

  const std::string& servingDocument(uint16_t index, uint32_t nowMs);
};

}  // namespace gmb

#endif

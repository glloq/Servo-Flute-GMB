#include "GmbSysExService.h"

#include <utility>

namespace gmb {

// Out-of-class definitions of the class constants: ODR-safe on the pre-C++17
// dialect the ESP32 Arduino toolchain builds this firmware with (same reason as
// the block at the top of GmbSysEx.cpp). Redundant from C++17 on.
#if __cplusplus < 201703L
constexpr uint32_t GmbSysExService::kTransferIdleMs;
constexpr int GmbSysExService::kMaxTokens;
constexpr uint32_t GmbSysExService::kRefillMs;
#endif

// The default snapshot renders the section 5.1 document: an instrument that is
// present but not configured. Building it here, before anything else can run,
// is what makes _descriptor non-null for the whole life of the service - the
// invariant descriptorJson() and descriptorSize() rest on. It is also the right
// degraded answer if the FIRST setSnapshot() of the boot never manages to
// publish: General-Midi-Boop reads "not configured" and hands the user manual
// entry, instead of a protocol error.
//
// make_shared rather than `new std::string(...)`: one allocation for the control
// block and the string object together instead of two, and no raw owning pointer
// in flight. Note what it does NOT buy - see setSnapshot() for why an allocation
// failure here cannot be turned into a null check.
GmbSysExService::GmbSysExService()
  : _descriptor(std::make_shared<std::string>(GmbDescriptor::toJson(CapabilitySnapshot()))),
    _servingLastMs(0),
    _servingDelivered(0),
    _handshakeFlags(kChangeNotificationSupported),
    _handled(0),
    _dropped(0),
    _rebuildFailures(0),
    _tokens(kMaxTokens),
    _lastRefillMs(0) {}

void GmbSysExService::setSnapshot(const CapabilitySnapshot& snapshot) {
  // BUILD FIRST, PUBLISH LAST. _snapshot and _descriptor are ONE pair as far as
  // the controller is concerned: the handshake announces _snapshot.revision and
  // the size of _descriptor, block 0x10 and GET /gmb/descriptor.json serve
  // _descriptor. Advancing one without the other is not a transient glitch, it
  // is permanent: General-Midi-Boop would cache the OLD document under the NEW
  // revision number, and since the revision only moves on the next real
  // configuration change it would never re-fetch. So everything that can fail
  // happens below OUT OF SIGHT of the controller, and the pair is handed over in
  // one step that cannot fail.
  std::shared_ptr<const std::string> next;
  try {
    // _staged still owns the buffers of the previous snapshot, so this copy
    // reuses them: measured 0 allocations in steady state, against 3 for a
    // staging copy built afresh on the stack each time.
    _staged = snapshot;
    std::string rendered = GmbDescriptor::toJson(_staged);
    // A rebuild that produces the very same bytes is not a change. Keeping the
    // document OBJECT then costs nothing and says so: no second copy of ~800
    // bytes, and a transfer in flight is not dropped by the
    // "_serving != _descriptor" rule of handleMessage() for a document that did
    // not move. GmbRuntime::onConfigurationActivated() takes this path on every
    // activation that changes nothing General-Midi-Boop is told about.
    if (*_descriptor == rendered) next = _descriptor;
    else next = std::make_shared<std::string>(std::move(rendered));
  } catch (...) {
    // This firmware really is compiled WITH exceptions, contrary to what the
    // "exceptions desactivees" shorthand elsewhere in the repository suggests:
    // framework-arduinoespressif32 2.0.17, tools/platformio-build-esp32.py,
    // CXXFLAGS = [..., "-std=gnu++11", "-fexceptions", "-fno-rtti"], over an
    // ESP-IDF built with CONFIG_COMPILER_CXX_EXCEPTIONS=y. So a std::string that
    // cannot find a contiguous block on a fragmented heap THROWS here; it does
    // not return null, and no amount of std::nothrow on this side would change
    // that, because the throw happens inside GmbDescriptor::toJson() before any
    // pointer of ours exists. Left uncaught it unwinds out of loop(), finds no
    // handler, and aborts the board - a reboot in the middle of a performance,
    // caused by a discovery document. Caught here it costs exactly one stale
    // descriptor. The descriptor is metadata; the note is not.
    next.reset();
  }

  if (!next) {
    // Degraded, visible, and NOT permanent: the pair is still the coherent one
    // it was, descriptorRebuildFailures() says the served document is behind the
    // active configuration, and the next activation rebuilds from scratch. Only
    // _staged is left half-written, and nothing ever reads it.
    _rebuildFailures++;
    return;
  }

  // Point of no return, and nothing here can fail: swapping two snapshots moves
  // std::string and std::vector buffers instead of allocating any, and assigning
  // a shared_ptr only touches a refcount. A chunk is therefore never served from
  // a half-built document, and a transfer already in flight keeps the document
  // it started on because that one is held alive by _serving.
  //
  // SWAP, not move-assign: a moved-from _staged would hand its buffers over and
  // start the next activation empty, which is exactly the allocation this member
  // exists to avoid. After the swap it holds the previous snapshot instead, with
  // the capacity that goes with it.
  std::swap(_snapshot, _staged);
  _descriptor = next;
}

void GmbSysExService::setHttpDescriptorAvailable(bool available) {
  if (available) _handshakeFlags |= kHttpDescriptorAvailable;
  else _handshakeFlags &= (uint8_t)~kHttpDescriptorAvailable;
}

bool GmbSysExService::allow(uint32_t nowMs) {
  if (_lastRefillMs == 0) _lastRefillMs = nowMs;
  uint32_t elapsed = nowMs - _lastRefillMs;
  if (elapsed >= kRefillMs) {
    int add = (int)(elapsed / kRefillMs);
    _tokens = (_tokens + add > kMaxTokens) ? kMaxTokens : _tokens + add;
    _lastRefillMs += (uint32_t)add * kRefillMs;
  }
  if (_tokens <= 0) return false;
  _tokens--;
  return true;
}

void GmbSysExService::endTransfer() {
  _serving.reset();
  _servingDelivered = 0;
}

void GmbSysExService::expireStaleTransfer(uint32_t nowMs) {
  // The controller gave up mid-way: release the pinned document so the next
  // transfer starts on the current one instead of a stale snapshot forever.
  if (_serving && (uint32_t)(nowMs - _servingLastMs) > kTransferIdleMs) endTransfer();
}

std::vector<uint8_t> GmbSysExService::serveDescriptorChunk(uint16_t index, uint32_t nowMs) {
  expireStaleTransfer(nowMs);

  // THE invariant of block 0x10: a transfer picks its document exactly once, when
  // it starts, and every later segment of that transfer is cut out of that same
  // document. A retry - of segment 0 as much as of any other - is part of the
  // transfer in flight, so it must NOT re-pin: re-pinning is what would let a
  // controller reassemble segment 0 of one revision with segment 1 of the next.
  const bool starting = !_serving;
  const std::shared_ptr<const std::string> doc = starting ? _descriptor : _serving;

  std::vector<uint8_t> out = GmbSysEx::encodeDescriptorChunk(*doc, index);
  if (out.empty()) {
    // Out-of-range segment: answered with silence, and it neither starts a
    // transfer nor keeps an existing one alive.
    return out;
  }

  if (starting) {
    _serving = doc;
    _servingDelivered = 0;
  }
  _servingLastMs = nowMs;
  _servingDelivered++;

  // The document has been delivered in full: release the pin so the NEXT transfer
  // starts on whatever document is current then. Both conditions matter - the
  // last segment alone would end the transfer of a controller that happened to
  // ask for the final segment first.
  const uint16_t total = GmbSysEx::chunkCount(doc->size());
  if ((uint32_t)index + 1u >= (uint32_t)total && _servingDelivered >= (uint32_t)total) {
    endTransfer();
  }
  return out;
}

std::vector<uint8_t> GmbSysExService::handleMessage(const uint8_t* data, size_t len,
                                                    uint32_t nowMs) {
  SysExRequest req = GmbSysEx::parseRequest(data, len);
  if (!req.valid) {
    // Malformed, truncated, wrong manufacturer / GMB id, 8-bit payload, unknown
    // block, or a response echoed back: ignored, no reply, no work.
    _dropped++;
    return std::vector<uint8_t>();
  }

  if (!allow(nowMs)) {
    _dropped++;
    return std::vector<uint8_t>();
  }
  _handled++;

  if (req.block == kBlockHandshake) {
    expireStaleTransfer(nowMs);
    // The handshake announces the CURRENT revision and descriptor_size. A transfer
    // still pinned to a document that is no longer the current one would answer
    // every following segment with bytes that contradict the frame we are about to
    // send, so it is dropped here rather than five seconds later on the idle
    // timeout. When the pinned document IS the current one this changes nothing.
    if (_serving && _serving != _descriptor) endTransfer();
    return GmbSysEx::encodeHandshake(_snapshot, descriptorSize(), _handshakeFlags);
  }
  if (req.block == kBlockDescriptorTransfer) {
    return serveDescriptorChunk(req.chunkIndex, nowMs);
  }
  return std::vector<uint8_t>();
}

}  // namespace gmb

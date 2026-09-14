#include "GmbSysExService.h"

namespace gmb {

// Out-of-class definitions of the class constants: ODR-safe on the pre-C++17
// dialect the ESP32 Arduino toolchain builds this firmware with (same reason as
// the block at the top of GmbSysEx.cpp). Redundant from C++17 on.
#if __cplusplus < 201703L
constexpr uint32_t GmbSysExService::kTransferIdleMs;
constexpr int GmbSysExService::kMaxTokens;
constexpr uint32_t GmbSysExService::kRefillMs;
#endif

GmbSysExService::GmbSysExService()
  : _descriptor(new std::string(GmbDescriptor::toJson(CapabilitySnapshot()))),
    _servingLastMs(0),
    _servingDelivered(0),
    _handshakeFlags(kChangeNotificationSupported),
    _handled(0),
    _dropped(0),
    _tokens(kMaxTokens),
    _lastRefillMs(0) {}

void GmbSysExService::setSnapshot(const CapabilitySnapshot& snapshot) {
  _snapshot = snapshot;
  // One render per activation. Published atomically: a chunk is never served from
  // a half-built document, and a transfer already in flight keeps the document it
  // started on because that one is held alive by _serving.
  _descriptor = std::shared_ptr<const std::string>(new std::string(GmbDescriptor::toJson(_snapshot)));
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

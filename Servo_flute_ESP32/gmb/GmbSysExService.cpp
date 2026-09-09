#include "GmbSysExService.h"

namespace gmb {

GmbSysExService::GmbSysExService()
  : _descriptor(new std::string(GmbDescriptor::toJson(CapabilitySnapshot()))),
    _servingLastMs(0),
    _handshakeFlags(kChangeNotificationSupported),
    _handled(0),
    _dropped(0),
    _tokens(kMaxTokens),
    _lastRefillMs(0) {}

void GmbSysExService::setSnapshot(const CapabilitySnapshot& snapshot) {
  _snapshot = snapshot;
  // One render per activation. Published atomically: a chunk is never served from
  // a half-built document.
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

const std::string& GmbSysExService::servingDocument(uint16_t index, uint32_t nowMs) {
  const bool stale = _serving && ((uint32_t)(nowMs - _servingLastMs) > kTransferIdleMs);
  // A transfer starts at segment 0; a stale pin (the controller gave up mid-way)
  // is released so the next transfer sees the current document.
  if (!_serving || index == 0 || stale) _serving = _descriptor;
  _servingLastMs = nowMs;
  return *_serving;
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
    return GmbSysEx::encodeHandshake(_snapshot, descriptorSize(), _handshakeFlags);
  }
  if (req.block == kBlockDescriptorTransfer) {
    const std::string& doc = servingDocument(req.chunkIndex, nowMs);
    std::vector<uint8_t> out = GmbSysEx::encodeDescriptorChunk(doc, req.chunkIndex);
    // Last segment delivered: release the pinned document so the next transfer
    // picks up the current one.
    if (!out.empty() && req.chunkIndex + 1 >= GmbSysEx::chunkCount(doc.size())) {
      _serving.reset();
    }
    return out;
  }
  return std::vector<uint8_t>();
}

}  // namespace gmb

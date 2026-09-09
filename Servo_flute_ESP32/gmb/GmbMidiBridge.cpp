#include "GmbMidiBridge.h"

#include <string.h>

#include "GmbSysExService.h"

namespace gmb {

GmbMidiBridge::GmbMidiBridge()
  : _service(0), _portCount(0), _pendingLen(0), _pendingPort(0),
    _staged(0), _overruns(0) {
  for (size_t i = 0; i < kMaxPorts; i++) _ports[i] = 0;
  memset(_pending, 0, sizeof(_pending));
}

void GmbMidiBridge::registerPort(IGmbMidiPort* port) {
  if (port == 0) return;
  for (size_t i = 0; i < _portCount; i++) {
    if (_ports[i] == port) return;
  }
  if (_portCount >= kMaxPorts) return;
  _ports[_portCount++] = port;
}

void GmbMidiBridge::onSysEx(IGmbMidiPort* from, const uint8_t* data, size_t len) {
  if (_service == 0 || data == 0) return;
  // Drop anything that is not shaped like a GMB request before it costs a copy.
  // A long or foreign SysEx (a sample dump, another vendor's message) is not our
  // business and must not displace a pending GMB request.
  if (len < 6 || len > GmbSysEx::kMaxRequestBytes) return;
  if (data[0] != GmbSysEx::kStart || data[1] != GmbSysEx::kManufacturer ||
      data[2] != GmbSysEx::kGmbId || data[len - 1] != GmbSysEx::kEnd) {
    return;
  }
  if (_pendingLen != 0) {
    // A reply is already owed; the loop has not run yet. Drop rather than queue.
    _overruns++;
    return;
  }
  memcpy(_pending, data, len);
  _pendingLen = len;
  _pendingPort = from;
  _staged++;
}

void GmbMidiBridge::service(uint32_t nowMs) {
  if (_service == 0 || _pendingLen == 0) return;

  // Take the request before answering, so a callback that fires while we build
  // the response can stage the next one.
  uint8_t frame[GmbSysEx::kMaxRequestBytes];
  memcpy(frame, _pending, _pendingLen);
  const size_t len = _pendingLen;
  IGmbMidiPort* port = _pendingPort;
  _pendingLen = 0;
  _pendingPort = 0;

  std::vector<uint8_t> response = _service->handleMessage(frame, len, nowMs);
  if (response.empty()) return;
  if (port == 0 || !port->canSendSysEx()) return;
  port->sendSysEx(response.data(), response.size());
}

void GmbMidiBridge::notifyCapabilitiesChanged(uint8_t changeFlags) {
  if (_service == 0) return;
  std::vector<uint8_t> msg = _service->notification(changeFlags);
  if (msg.empty()) return;
  for (size_t i = 0; i < _portCount; i++) {
    if (_ports[i] != 0 && _ports[i]->canSendSysEx()) {
      _ports[i]->sendSysEx(msg.data(), msg.size());
    }
  }
}

}  // namespace gmb

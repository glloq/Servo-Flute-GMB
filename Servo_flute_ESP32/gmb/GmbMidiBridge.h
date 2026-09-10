/***********************************************************************************************
 * GmbMidiBridge - routes SysEx between the MIDI transports and GmbSysExService.
 *
 *     MIDI transport -> SysEx message -> GmbSysExService -> response bytes
 *                    -> same originating transport
 *
 * A request arriving in a transport callback is only STAGED there: the reply is
 * built and sent from the main loop (service()). That keeps the MIDI parse
 * callback free of allocation and of re-entrant writes into the transport it was
 * called from, and it keeps GMB discovery - which is control-plane traffic - off
 * the note path.
 *
 * One pending request at a time. A second request arriving before the first is
 * answered is dropped, which bounds what a flood of SysEx traffic can cost to a
 * single fixed-size buffer.
 ***********************************************************************************************/
#ifndef GMB_MIDI_BRIDGE_H
#define GMB_MIDI_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#include "GmbMidiPort.h"
#include "GmbSysEx.h"

namespace gmb {

class GmbSysExService;

class GmbMidiBridge {
public:
  static constexpr size_t kMaxPorts = 4;

  GmbMidiBridge();

  void begin(GmbSysExService* service) { _service = service; }
  void registerPort(IGmbMidiPort* port);

  // Called from a transport's SysEx callback with one complete message.
  // Copies the frame and returns immediately; nothing is answered here.
  void onSysEx(IGmbMidiPort* from, const uint8_t* data, size_t len);

  // Called from the main loop: answers a staged request, if any.
  void service(uint32_t nowMs);

  // Send a block 0x11 change notification to every port that can carry it.
  void notifyCapabilitiesChanged(uint8_t changeFlags);

  uint32_t stagedCount() const { return _staged; }
  uint32_t overrunCount() const { return _overruns; }

private:
  GmbSysExService* _service;
  IGmbMidiPort* _ports[kMaxPorts];
  size_t _portCount;

  // Single pending request. Requests are 6 or 8 bytes; anything longer is not a
  // GMB request and is dropped before it is copied.
  uint8_t _pending[GmbSysEx::kMaxRequestBytes];
  size_t _pendingLen;
  IGmbMidiPort* _pendingPort;

  uint32_t _staged;
  uint32_t _overruns;
};

}  // namespace gmb

#endif

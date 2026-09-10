/***********************************************************************************************
 * IGmbMidiPort - what a MIDI transport must offer to take part in GMB discovery.
 *
 * The GMB protocol logic lives once, in GmbSysExService. A transport only has to
 * say whether it can currently send SysEx back, and how to write bytes. A
 * transport with no return path (the DIN input, which is RX only on this board)
 * simply never implements this interface and keeps working normally for
 * Note / CC traffic.
 ***********************************************************************************************/
#ifndef GMB_MIDI_PORT_H
#define GMB_MIDI_PORT_H

#include <stddef.h>
#include <stdint.h>

namespace gmb {

class IGmbMidiPort {
public:
  virtual ~IGmbMidiPort() {}

  // True when a peer is connected and a SysEx reply can actually be delivered.
  virtual bool canSendSysEx() const = 0;

  // Send one complete SysEx message (F0 ... F7).
  virtual void sendSysEx(const uint8_t* data, size_t len) = 0;

  // Short stable name, for logs and diagnostics ("ble", "rtpmidi").
  virtual const char* gmbPortName() const = 0;
};

}  // namespace gmb

#endif

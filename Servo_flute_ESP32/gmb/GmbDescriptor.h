/***********************************************************************************************
 * GmbDescriptor - GMB v2 JSON capability descriptor serialiser.
 *
 * Renders one immutable CapabilitySnapshot as the descriptor document defined in
 * GMB docs/SYSEX_IDENTITY.md section 5. The output is restricted to 7-bit ASCII
 * (any non-ASCII code point is escaped as \uXXXX), so the very same bytes are
 * safe to carry over the block 0x10 SysEx transfer AND to serve from
 * GET /gmb/descriptor.json - the two can never diverge because they are the same
 * cached string.
 *
 * Pure core: written with std::string rather than ArduinoJson so the document is
 * host-testable and costs no extra heap churn on the ESP32.
 ***********************************************************************************************/
#ifndef GMB_DESCRIPTOR_H
#define GMB_DESCRIPTOR_H

#include <string>

#include "Capabilities.h"

namespace gmb {

class GmbDescriptor {
public:
  // Serialise `snapshot` as a GMB v2 descriptor (one logical instrument).
  // Always 7-bit ASCII, always a complete JSON document.
  static std::string toJson(const CapabilitySnapshot& snapshot);
};

}  // namespace gmb

#endif

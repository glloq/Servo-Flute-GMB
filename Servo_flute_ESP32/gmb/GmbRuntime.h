/***********************************************************************************************
 * GmbRuntime - the firmware-side facade for General-Midi-Boop recognition.
 *
 * One place that owns the GMB objects and the ESP32-specific bits the pure core
 * deliberately does not know about:
 *   - the stable instance id, derived from the eFuse MAC;
 *   - the persistent capability revision, stored in NVS (never in config.json, so
 *     a configuration save never rewrites the counter and a counter bump never
 *     rewrites the configuration);
 *   - the capability snapshot and the cached JSON descriptor.
 *
 * Call order:
 *   begin()                      once at boot, after the configuration is loaded
 *                                and validated
 *   onConfigurationActivated()   after a new configuration has been validated,
 *                                committed and made ACTIVE
 *
 * onConfigurationActivated() is the only capability-change entry point: it
 * recomputes the snapshot, bumps the revision if and only if the announced
 * capabilities really moved, rebuilds the cached descriptor, and emits block 0x11.
 * A change that requires a reboot must NOT be reported here - it is not active
 * yet, and the next begin() picks it up.
 ***********************************************************************************************/
#ifndef GMB_RUNTIME_H
#define GMB_RUNTIME_H

#include <stdint.h>

#include <string>

#include "GmbMidiBridge.h"
#include "GmbRevision.h"
#include "GmbSysExService.h"

namespace gmb {
namespace runtime {

// Boot-time initialisation. `configValidated` is the validation result of the
// configuration that is actually active.
void begin(bool configValidated);

// Report that a new configuration is validated, committed and ACTIVE. Rebuilds
// the snapshot and the descriptor, and notifies General-Midi-Boop when the
// announced capabilities really changed. Call it only once all four are true -
// a configuration that failed validation, failed to persist, or still needs a
// reboot has not been activated and must not be announced.
void onConfigurationActivated();

// The web server (Wi-Fi mode only) toggles handshake flag bit 0.
void setHttpDescriptorAvailable(bool available);

GmbSysExService& service();
GmbMidiBridge& bridge();

// Convenience accessors for the web API / diagnostics.
const std::string& descriptorJson();
uint32_t revision();
uint32_t instanceId();
bool isConfigured();

}  // namespace runtime
}  // namespace gmb

#endif

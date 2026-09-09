#include "GmbRuntime.h"

#include <Arduino.h>
#include <Preferences.h>

#include "../ConfigStorage.h"
#include "GmbInstanceId.h"

namespace gmb {
namespace runtime {

namespace {

// NVS namespace and keys. Kept out of /config.json on purpose: the counter must
// survive a factory reset of the configuration (the board is still the same
// exemplar), and a configuration save must not rewrite the counter.
const char* kNvsNamespace = "gmb";
const char* kKeyRevision = "rev";
const char* kKeySignature = "sig";

GmbSysExService g_service;
GmbMidiBridge g_bridge;
RevisionTracker g_revision;
uint32_t g_instanceId = 0;
bool g_begun = false;

void persist(uint32_t revision, uint32_t signature) {
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, false)) {
    if (DEBUG) Serial.println("ERREUR: GMB - NVS indisponible, revision non persistee");
    return;
  }
  prefs.putUInt(kKeyRevision, revision);
  prefs.putUInt(kKeySignature, signature);
  prefs.end();
}

// Build the snapshot for the active configuration, stamp the identity that does
// not come from the configuration, and publish it (which rebuilds the descriptor).
CapabilitySnapshot buildActiveSnapshot(bool configValidated, uint32_t revision) {
  CapabilitySnapshot snap = buildSnapshot(cfg, configValidated);
  snap.identity.instanceId = g_instanceId;
  snap.revision = revision;
  return snap;
}

}  // namespace

void begin(bool configValidated) {
  // Stable per-board identity. Two boards flashed with the same binary must never
  // share it, so it is derived from the eFuse MAC rather than from the profile.
  g_instanceId = instanceIdFromHardwareId(ESP.getEfuseMac());

  uint32_t storedRevision = 0;
  uint32_t storedSignature = 0;
  {
    Preferences prefs;
    if (prefs.begin(kNvsNamespace, true)) {
      storedRevision = prefs.getUInt(kKeyRevision, 0);
      storedSignature = prefs.getUInt(kKeySignature, 0);
      prefs.end();
    }
  }

  CapabilitySnapshot probe = buildActiveSnapshot(configValidated, storedRevision);
  CapabilitySignature signature = computeSignature(probe);
  // A plain reboot leaves the signature untouched and writes nothing to flash.
  if (g_revision.begin(storedRevision, storedSignature, signature)) {
    persist(g_revision.revision(), g_revision.signature());
  }

  probe.revision = g_revision.revision();
  g_service.setSnapshot(probe);
  g_bridge.begin(&g_service);
  g_begun = true;

  if (DEBUG) {
    Serial.print("DEBUG: GMB - instance_id 0x");
    Serial.print(g_instanceId, HEX);
    Serial.print(" revision ");
    Serial.print(g_revision.revision());
    Serial.print(" descripteur ");
    Serial.print((unsigned)g_service.descriptorSize());
    Serial.print(" octets, configured=");
    Serial.println(g_service.snapshot().instrument.configured ? "true" : "false");
  }
}

void onConfigurationActivated() {
  if (!g_begun) return;

  // The caller guarantees the configuration validated, was committed and is now
  // active, so the snapshot is built as a configured instrument.
  CapabilitySnapshot next = buildActiveSnapshot(true, g_revision.revision());
  CapabilitySignature signature = computeSignature(next);

  // restartRequired is intentionally false here: a change that needs a reboot is
  // not active, so it is never reported through this path.
  if (!g_revision.onConfigurationActivated(signature, false)) {
    // Nothing GMB is told about changed: no flash write, no notification. The
    // snapshot is still republished so any non-announced field stays coherent.
    next.revision = g_revision.revision();
    g_service.setSnapshot(next);
    return;
  }

  persist(g_revision.revision(), g_revision.signature());
  next.revision = g_revision.revision();
  // Rebuild the cached descriptor BEFORE announcing the new revision, so a
  // controller that reacts immediately always finds the matching document.
  g_service.setSnapshot(next);
  g_bridge.notifyCapabilitiesChanged(g_revision.changeFlags());

  if (DEBUG) {
    Serial.print("DEBUG: GMB - capacites modifiees, revision ");
    Serial.print(g_revision.revision());
    Serial.print(" flags 0x");
    Serial.println(g_revision.changeFlags(), HEX);
  }
}

void setHttpDescriptorAvailable(bool available) {
  g_service.setHttpDescriptorAvailable(available);
}

GmbSysExService& service() { return g_service; }
GmbMidiBridge& bridge() { return g_bridge; }
const std::string& descriptorJson() { return g_service.descriptorJson(); }
uint32_t revision() { return g_revision.revision(); }
uint32_t instanceId() { return g_instanceId; }
bool isConfigured() { return g_service.snapshot().instrument.configured; }

}  // namespace runtime
}  // namespace gmb

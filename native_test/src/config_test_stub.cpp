#include "ConfigStorage.h"
RuntimeConfig cfg{};
ConfigValidationResult validateAndNormalizeConfig(RuntimeConfig& config, const RuntimeConfig*) {
  ConfigValidationResult r{true,false,false,"",""};
  if (config.numPumps > MAX_PUMPS) { config.numPumps = MAX_PUMPS; r.corrected=true; r.warnings += "numPumps corrected"; }
  if (config.pumpCascadeThreshold > 99) { config.pumpCascadeThreshold=99; r.corrected=true; r.warnings += " cascade corrected"; }
  for (uint8_t i=0;i<MAX_PUMPS;i++) if (config.pumpMinPwm[i] > config.pumpMaxPwm[i]) { r.valid=false; r.error="pump range inverted"; }
  if (config.sensorMinMm >= config.sensorMaxMm) { r.valid=false; r.error="sensor range invalid"; }
  if (config.hallThresholdLow >= config.hallThresholdHigh) { r.valid=false; r.error="hall range invalid"; }
  return r;
}

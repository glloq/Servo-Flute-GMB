#include "ConfigStorage.h"

static void appendIssue(String& dst, const String& msg) {
  if (dst.length() > 0) dst += "; ";
  dst += msg;
}

static bool isReservedGpio(uint8_t pin) {
  return pin == STATUS_LED_PIN || pin == PAIRING_BUTTON_PIN || pin == MODE_SWITCH_PIN ||
         pin == I2C_SDA_PIN || pin == I2C_SCL_PIN || pin == PIN_SERVOS_OFF || pin == 1 || pin == 3;
}

static bool isInputOnlyGpio(uint8_t pin) {
  return pin == 34 || pin == 35 || pin == 36 || pin == 39;
}

static bool normalizeRangeU8(uint8_t& v, uint8_t lo, uint8_t hi) {
  uint8_t old = v;
  if (v < lo) v = lo;
  if (v > hi) v = hi;
  return v != old;
}


bool modeUsesPhysicalValve(uint8_t airMode) {
  return airMode == AIR_MODE_SOLENOID_SERVO ||
         airMode == AIR_MODE_SERVO_VALVE ||
         airMode == AIR_MODE_PUMP_VALVE ||
         airMode == AIR_MODE_PUMP_RESERVOIR;
}

bool configurationUsesSolenoidValve(const RuntimeConfig& config) {
  return modeUsesPhysicalValve(config.airMode) && config.valveType == 0;
}

bool configurationUsesFan(const RuntimeConfig& config) {
  return config.airMode == AIR_MODE_FAN_SERVO;
}

bool configurationUsesPumps(const RuntimeConfig& config) {
  return config.airMode == AIR_MODE_PUMP_VALVE || config.airMode == AIR_MODE_PUMP_RESERVOIR;
}

bool configurationUsesReservoirSensor(const RuntimeConfig& config) {
  return config.airMode == AIR_MODE_PUMP_RESERVOIR;
}

static bool normalizeRangeU16(uint16_t& v, uint16_t lo, uint16_t hi) {
  uint16_t old = v;
  if (v < lo) v = lo;
  if (v > hi) v = hi;
  return v != old;
}

ConfigValidationResult validateAndNormalizeConfig(RuntimeConfig& config, const RuntimeConfig* previousConfig) {
  ConfigValidationResult r{true, false, false, "", ""};

  r.corrected |= normalizeRangeU8(config.numFingers, 1, MAX_FINGER_SERVOS);
  r.corrected |= normalizeRangeU8(config.numNotes, 1, MAX_NOTES);
  r.corrected |= normalizeRangeU8(config.airflowPcaChannel, 0, 31);
  r.corrected |= normalizeRangeU8(config.fingerAngleOpen, 0, 180);
  r.corrected |= normalizeRangeU8(config.halfHolePercent, 0, 100);
  r.corrected |= normalizeRangeU8(config.midiChannel, 0, 16);
  r.corrected |= normalizeRangeU16(config.servoToSolenoidDelayMs, 0, CONFIG_MAX_SERVO_DELAY_MS);
  r.corrected |= normalizeRangeU16(config.minNoteDurationMs, CONFIG_MIN_NOTE_DURATION_LIMIT_MS, CONFIG_MAX_NOTE_DURATION_LIMIT_MS);
  r.corrected |= normalizeRangeU16(config.servoAirflowOff, 0, 180);
  r.corrected |= normalizeRangeU16(config.servoAirflowMin, 0, 180);
  r.corrected |= normalizeRangeU16(config.servoAirflowMax, 0, 180);
  r.corrected |= normalizeRangeU16(config.servoAngleOff, 0, 180);
  r.corrected |= normalizeRangeU16(config.servoAngleMin, 0, 180);
  r.corrected |= normalizeRangeU16(config.servoAngleMax, 0, 180);
  r.corrected |= normalizeRangeU8(config.angleServoPcaChannel, 0, 31);
  r.corrected |= normalizeRangeU8(config.valveServoPcaChannel, 0, 31);
  r.corrected |= normalizeRangeU8(config.valveServoCloseAngle, 0, 180);
  r.corrected |= normalizeRangeU8(config.valveServoOpenAngle, 0, 180);
  r.corrected |= normalizeRangeU8(config.airMode, AIR_MODE_SOLENOID_SERVO, AIR_MODE_PUMP_RESERVOIR);
  r.corrected |= normalizeRangeU8(config.valveType, 0, 1);
  r.corrected |= normalizeRangeU8(config.motorType, MOTOR_TYPE_PWM, MOTOR_TYPE_ONOFF);
  r.corrected |= normalizeRangeU8(config.sensorType, SENSOR_TYPE_TOF_VL53L0X, SENSOR_TYPE_ENDSTOP_OPT);
  r.corrected |= normalizeRangeU8(config.numPumps, 1, MAX_PUMPS);
  r.corrected |= normalizeRangeU8(config.pumpCascadeThreshold, 0, 99);
  r.corrected |= normalizeRangeU8(config.bangbangHysteresis, 0, 50);
  r.corrected |= normalizeRangeU8(config.fanMinPwm, 0, 255);
  r.corrected |= normalizeRangeU8(config.fanMaxPwm, 0, 255);
  r.corrected |= normalizeRangeU8(config.fanIdlePercent, 0, 100);
  r.corrected |= normalizeRangeU8(config.fanDefaultPercent, 0, 100);
  r.corrected |= normalizeRangeU8(config.fanMaxNotePercent, 0, 100);
  r.corrected |= normalizeRangeU8(config.pumpDirectIdlePercent, 0, 100);
  r.corrected |= normalizeRangeU8(config.pumpDirectMaxPercent, 0, 100);
  r.corrected |= normalizeRangeU8(config.reservoirTargetPercent, 0, 100);
  r.corrected |= normalizeRangeU8(config.airAttackMode, 0, 2);
  r.corrected |= normalizeRangeU8(config.airAttackOffset, 0, 50);
  r.corrected |= normalizeRangeU8(config.airVelocityResponse, 0, 100);
  r.corrected |= normalizeRangeU8(config.solenoidPwmActivation, 0, 255);
  r.corrected |= normalizeRangeU8(config.solenoidPwmHolding, 0, 255);

  if (config.servoAirflowMin >= config.servoAirflowMax) appendIssue(r.error, "servoAirflowMin must be < servoAirflowMax");
  if (config.servoAngleMin >= config.servoAngleMax) appendIssue(r.error, "servoAngleMin must be < servoAngleMax");
  if (config.fanMinPwm > config.fanMaxPwm) appendIssue(r.error, "fanMinPwm must be <= fanMaxPwm");
  if (configurationUsesReservoirSensor(config) && config.sensorType == SENSOR_TYPE_HALL_KY024 &&
      config.hallThresholdLow >= config.hallThresholdHigh) appendIssue(r.error, "hallThresholdLow must be < hallThresholdHigh");
  if (configurationUsesReservoirSensor(config) &&
      (config.sensorType == SENSOR_TYPE_TOF_VL53L0X || config.sensorType == SENSOR_TYPE_TOF_VL6180X) &&
      !(config.sensorMinMm < config.sensorTargetMm && config.sensorTargetMm < config.sensorMaxMm)) appendIssue(r.error, "sensorMinMm < sensorTargetMm < sensorMaxMm required");

  if (!modeUsesPhysicalValve(config.airMode) && config.valveType == 1) appendIssue(r.warnings, "valveType ignored by selected airMode");
  if (config.airMode == AIR_MODE_SERVO_VALVE && config.valveType != 1) appendIssue(r.error, "airMode servo-valve requires valveType=servo");
  if (configurationUsesPumps(config) && config.numPumps < 1) appendIssue(r.error, "pump mode requires at least one pump");
  if (!configurationUsesReservoirSensor(config) && config.sensorType != DEFAULT_SENSOR_TYPE) appendIssue(r.warnings, "reservoir sensor ignored by selected airMode");

  bool pcaUsed[32] = {false};
  for (uint8_t i = 0; i < config.numFingers; i++) {
    r.corrected |= normalizeRangeU8(config.fingers[i].pcaChannel, 0, 31);
    r.corrected |= normalizeRangeU16(config.fingers[i].closedAngle, 0, 180);
    if (config.fingers[i].direction != -1 && config.fingers[i].direction != 1) { config.fingers[i].direction = 1; r.corrected = true; }
    r.corrected |= normalizeRangeU8(config.fingers[i].halfPercent, 0, 100);
    if (pcaUsed[config.fingers[i].pcaChannel]) appendIssue(r.error, "duplicate finger PCA channel " + String(config.fingers[i].pcaChannel));
    pcaUsed[config.fingers[i].pcaChannel] = true;
  }
  if (pcaUsed[config.airflowPcaChannel]) appendIssue(r.error, "airflow servo PCA channel conflicts with finger");
  pcaUsed[config.airflowPcaChannel] = true;
  if (modeUsesPhysicalValve(config.airMode) && config.valveType == 1) {
    if (pcaUsed[config.valveServoPcaChannel]) appendIssue(r.error, "valve servo PCA channel conflicts with another servo");
    pcaUsed[config.valveServoPcaChannel] = true;
  }
  if (config.angleServoEnabled) {
    if (pcaUsed[config.angleServoPcaChannel]) appendIssue(r.error, "angle servo PCA channel conflicts with another servo");
    pcaUsed[config.angleServoPcaChannel] = true;
  }

  bool midiSeen[128] = {false};
  for (uint8_t i = 0; i < config.numNotes; i++) {
    uint16_t midiNote = config.notes[i].midiNote;
    if (midiNote > 127) {
      appendIssue(r.error, "MIDI note out of range at index " + String(i) + ": " + String(midiNote));
    } else {
      if (midiSeen[midiNote]) appendIssue(r.error, "duplicate MIDI note " + String(midiNote));
      midiSeen[midiNote] = true;
    }
    r.corrected |= normalizeRangeU8(config.notes[i].airflowMinPercent, 0, 100);
    r.corrected |= normalizeRangeU8(config.notes[i].airflowMaxPercent, 0, 100);
    r.corrected |= normalizeRangeU8(config.notes[i].airflowNominalPercent, 0, 100);
    r.corrected |= normalizeRangeU8(config.notes[i].anglePercent, 0, 100);
    if (config.notes[i].airflowMinPercent > config.notes[i].airflowMaxPercent) appendIssue(r.error, "note airflow min > max at index " + String(i));
    // Enforce 0 <= min <= nominal <= max <= 100.
    if (config.notes[i].airflowNominalPercent < config.notes[i].airflowMinPercent) appendIssue(r.error, "note airflow nominal < min at index " + String(i));
    if (config.notes[i].airflowNominalPercent > config.notes[i].airflowMaxPercent) appendIssue(r.error, "note airflow nominal > max at index " + String(i));
    for (uint8_t f = 0; f < config.numFingers; f++) if (config.notes[i].fingerPattern[f] > 2) appendIssue(r.error, "fingerPattern must be 0, 1, or 2");
  }
  for (uint8_t i = 0; i < MAX_PUMPS; i++) {
    r.corrected |= normalizeRangeU8(config.pumpMinPwm[i], 0, 255);
    r.corrected |= normalizeRangeU8(config.pumpMaxPwm[i], 0, 255);
    if (config.pumpMinPwm[i] > config.pumpMaxPwm[i]) appendIssue(r.error, "pump min PWM > max PWM at index " + String(i));
  }

  uint8_t gpios[8]; uint8_t gcount = 0;
  if (configurationUsesSolenoidValve(config)) gpios[gcount++] = config.solenoidPin;
  if (configurationUsesFan(config)) gpios[gcount++] = config.fanPin;
  if (configurationUsesPumps(config)) for (uint8_t i = 0; i < config.numPumps && i < MAX_PUMPS; i++) gpios[gcount++] = config.pumpPins[i];
  if (configurationUsesReservoirSensor(config) && config.sensorType == SENSOR_TYPE_HALL_KY024) gpios[gcount++] = config.hallPin;
  if (configurationUsesReservoirSensor(config) && (config.sensorType == SENSOR_TYPE_ENDSTOP_MECH || config.sensorType == SENSOR_TYPE_ENDSTOP_OPT)) gpios[gcount++] = config.endstopPin;
  for (uint8_t i = 0; i < gcount; i++) {
    if (gpios[i] > CONFIG_MAX_GPIO) appendIssue(r.error, "GPIO out of range: " + String(gpios[i]));
    if (isReservedGpio(gpios[i])) appendIssue(r.error, "GPIO reserved by board function: " + String(gpios[i]));
    bool outputUse = (configurationUsesSolenoidValve(config) && gpios[i] == config.solenoidPin) || (configurationUsesFan(config) && gpios[i] == config.fanPin);
    for (uint8_t p = 0; p < config.numPumps && p < MAX_PUMPS; p++) if (gpios[i] == config.pumpPins[p]) outputUse = true;
    if (outputUse && isInputOnlyGpio(gpios[i])) appendIssue(r.error, "input-only GPIO used as output: " + String(gpios[i]));
    for (uint8_t j = i + 1; j < gcount; j++) if (gpios[i] == gpios[j]) appendIssue(r.error, "duplicate incompatible GPIO: " + String(gpios[i]));
  }

  if (previousConfig) {
    if (previousConfig->numFingers != config.numFingers || previousConfig->airflowPcaChannel != config.airflowPcaChannel ||
        previousConfig->valveServoPcaChannel != config.valveServoPcaChannel || previousConfig->angleServoPcaChannel != config.angleServoPcaChannel ||
        previousConfig->airMode != config.airMode || previousConfig->valveType != config.valveType || previousConfig->solenoidPin != config.solenoidPin ||
        previousConfig->fanPin != config.fanPin || previousConfig->numPumps != config.numPumps || previousConfig->motorType != config.motorType ||
        previousConfig->sensorType != config.sensorType || previousConfig->hallPin != config.hallPin || previousConfig->endstopPin != config.endstopPin ||
        previousConfig->serialMidiEnabled != config.serialMidiEnabled || previousConfig->serialMidiRxPin != config.serialMidiRxPin) {
      r.restartRequired = true;
    }
    for (uint8_t i = 0; i < config.numFingers && i < previousConfig->numFingers; i++) if (previousConfig->fingers[i].pcaChannel != config.fingers[i].pcaChannel) r.restartRequired = true;
    for (uint8_t i = 0; i < config.numPumps && i < previousConfig->numPumps; i++) if (previousConfig->pumpPins[i] != config.pumpPins[i]) r.restartRequired = true;
  }

  r.valid = (r.error.length() == 0);
  return r;
}

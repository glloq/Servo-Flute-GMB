#include "ConfigStorage.h"
#include <ArduinoJson.h>
#include <LittleFS.h>

// Instance globale
RuntimeConfig cfg;

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
  if (config.hallThresholdLow >= config.hallThresholdHigh) appendIssue(r.error, "hallThresholdLow must be < hallThresholdHigh");
  if (!(config.sensorMinMm < config.sensorTargetMm && config.sensorTargetMm < config.sensorMaxMm)) appendIssue(r.error, "sensorMinMm < sensorTargetMm < sensorMaxMm required");

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
  if (config.valveType == 1) {
    if (pcaUsed[config.valveServoPcaChannel]) appendIssue(r.error, "valve servo PCA channel conflicts with another servo");
    pcaUsed[config.valveServoPcaChannel] = true;
  }
  if (config.angleServoEnabled) {
    if (pcaUsed[config.angleServoPcaChannel]) appendIssue(r.error, "angle servo PCA channel conflicts with another servo");
    pcaUsed[config.angleServoPcaChannel] = true;
  }

  bool midiSeen[128] = {false};
  for (uint8_t i = 0; i < config.numNotes; i++) {
    if (midiSeen[config.notes[i].midiNote]) appendIssue(r.error, "duplicate MIDI note " + String(config.notes[i].midiNote));
    midiSeen[config.notes[i].midiNote] = true;
    r.corrected |= normalizeRangeU8(config.notes[i].airflowMinPercent, 0, 100);
    r.corrected |= normalizeRangeU8(config.notes[i].airflowMaxPercent, 0, 100);
    r.corrected |= normalizeRangeU8(config.notes[i].anglePercent, 0, 100);
    if (config.notes[i].airflowMinPercent > config.notes[i].airflowMaxPercent) appendIssue(r.error, "note airflow min > max at index " + String(i));
    for (uint8_t f = 0; f < config.numFingers; f++) if (config.notes[i].fingerPattern[f] > 2) appendIssue(r.error, "fingerPattern must be 0, 1, or 2");
  }
  for (uint8_t i = 0; i < MAX_PUMPS; i++) {
    r.corrected |= normalizeRangeU8(config.pumpMinPwm[i], 0, 255);
    r.corrected |= normalizeRangeU8(config.pumpMaxPwm[i], 0, 255);
    if (config.pumpMinPwm[i] > config.pumpMaxPwm[i]) appendIssue(r.error, "pump min PWM > max PWM at index " + String(i));
  }

  uint8_t gpios[8]; uint8_t gcount = 0;
  gpios[gcount++] = config.solenoidPin;
  if (config.airMode == AIR_MODE_FAN_SERVO) gpios[gcount++] = config.fanPin;
  for (uint8_t i = 0; i < config.numPumps && i < MAX_PUMPS; i++) gpios[gcount++] = config.pumpPins[i];
  if (config.sensorType == SENSOR_TYPE_HALL_KY024) gpios[gcount++] = config.hallPin;
  if (config.sensorType == SENSOR_TYPE_ENDSTOP_MECH || config.sensorType == SENSOR_TYPE_ENDSTOP_OPT) gpios[gcount++] = config.endstopPin;
  for (uint8_t i = 0; i < gcount; i++) {
    if (gpios[i] > CONFIG_MAX_GPIO) appendIssue(r.error, "GPIO out of range: " + String(gpios[i]));
    if (isReservedGpio(gpios[i])) appendIssue(r.error, "GPIO reserved by board function: " + String(gpios[i]));
    bool outputUse = gpios[i] == config.solenoidPin || gpios[i] == config.fanPin;
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

void ConfigStorage::initDefaults() {
  // --- Instrument ---
  cfg.numFingers = DEFAULT_NUM_FINGERS;
  cfg.numNotes = DEFAULT_NUM_NOTES;
  cfg.airflowPcaChannel = DEFAULT_AIRFLOW_PCA_CHANNEL;
  cfg.fingerAngleOpen = ANGLE_OPEN;
  cfg.halfHolePercent = 50;
  strncpy(cfg.embouchure, "trav", sizeof(cfg.embouchure));

  // Zero-fill all arrays first
  memset(cfg.fingers, 0, sizeof(cfg.fingers));
  memset(cfg.notes, 0, sizeof(cfg.notes));

  // Load default finger configs
  for (int i = 0; i < DEFAULT_NUM_FINGERS && i < MAX_FINGER_SERVOS; i++) {
    cfg.fingers[i].pcaChannel = DEFAULT_FINGERS[i].pcaChannel;
    cfg.fingers[i].closedAngle = DEFAULT_FINGERS[i].closedAngle;
    cfg.fingers[i].direction = DEFAULT_FINGERS[i].direction;
    cfg.fingers[i].isThumbHole = DEFAULT_FINGERS[i].isThumbHole;
    cfg.fingers[i].halfPercent = 0;  // 0 = use global halfHolePercent
  }

  // Load default note configs
  for (int i = 0; i < DEFAULT_NUM_NOTES && i < MAX_NOTES; i++) {
    cfg.notes[i].midiNote = DEFAULT_NOTES[i].midiNote;
    cfg.notes[i].airflowMinPercent = DEFAULT_NOTES[i].airflowMinPercent;
    cfg.notes[i].airflowMaxPercent = DEFAULT_NOTES[i].airflowMaxPercent;
    cfg.notes[i].anglePercent = DEFAULT_NOTES[i].anglePercent;
    // Copy finger pattern (default has DEFAULT_NUM_FINGERS, pad rest with 0)
    for (int f = 0; f < MAX_FINGER_SERVOS; f++) {
      cfg.notes[i].fingerPattern[f] = (f < DEFAULT_NUM_FINGERS) ? DEFAULT_NOTES[i].fingerPattern[f] : 0;
    }
  }

  // --- MIDI ---
  cfg.midiChannel = MIDI_CHANNEL;

  // --- Serial MIDI ---
  cfg.serialMidiEnabled = DEFAULT_SERIAL_MIDI_ENABLED;
  cfg.serialMidiRxPin = DEFAULT_SERIAL_MIDI_RX_PIN;

  // --- Timing ---
  cfg.servoToSolenoidDelayMs = SERVO_TO_SOLENOID_DELAY_MS;
  cfg.minNoteIntervalForValveCloseMs = MIN_NOTE_INTERVAL_FOR_VALVE_CLOSE_MS;
  cfg.minNoteDurationMs = MIN_NOTE_DURATION_MS;

  // --- Airflow ---
  cfg.servoAirflowOff = SERVO_AIRFLOW_OFF;
  cfg.servoAirflowMin = SERVO_AIRFLOW_MIN;
  cfg.servoAirflowMax = SERVO_AIRFLOW_MAX;

  // --- Angle servo (trav) ---
  cfg.servoAngleOff = SERVO_ANGLE_OFF;
  cfg.servoAngleMin = SERVO_ANGLE_MIN;
  cfg.servoAngleMax = SERVO_ANGLE_MAX;

  // --- Vibrato ---
  cfg.vibratoFrequencyHz = VIBRATO_FREQUENCY_HZ;
  cfg.vibratoMaxAmplitudeDeg = VIBRATO_MAX_AMPLITUDE_DEG;

  // --- CC defaults ---
  cfg.ccVolumeDefault = CC_VOLUME_DEFAULT;
  cfg.ccExpressionDefault = CC_EXPRESSION_DEFAULT;
  cfg.ccModulationDefault = CC_MODULATION_DEFAULT;
  cfg.ccBreathDefault = CC_BREATH_DEFAULT;
  cfg.ccBrightnessDefault = CC_BRIGHTNESS_DEFAULT;

  // --- CC2 ---
  cfg.cc2Enabled = CC2_ENABLED;
  cfg.cc2SilenceThreshold = CC2_SILENCE_THRESHOLD;
  cfg.cc2ResponseCurve = CC2_RESPONSE_CURVE;
  cfg.cc2TimeoutMs = CC2_TIMEOUT_MS;

  // --- Solenoide ---
  cfg.solenoidPwmActivation = SOLENOID_PWM_ACTIVATION;
  cfg.solenoidPwmHolding = SOLENOID_PWM_HOLDING;
  cfg.solenoidActivationTimeMs = SOLENOID_ACTIVATION_TIME_MS;

  // --- Expression airflow ---
  cfg.airAttackMode = 0;           // stable par defaut
  cfg.airAttackOffset = 20;        // 20% d'ecart
  cfg.airAttackMs = 150;           // 150ms transition
  cfg.airVelocityResponse = 50;    // 50% d'influence velocite

  // --- WiFi ---
  memset(cfg.wifiSsid, 0, sizeof(cfg.wifiSsid));
  memset(cfg.wifiPassword, 0, sizeof(cfg.wifiPassword));

  // --- Device ---
  strncpy(cfg.deviceName, DEVICE_NAME, sizeof(cfg.deviceName) - 1);
  cfg.deviceName[sizeof(cfg.deviceName) - 1] = '\0';

  // --- Power ---
  cfg.timeUnpower = TIMEUNPOWER;

  // --- Air delivery system (modulaire) ---
  cfg.airMode = DEFAULT_AIR_MODE;
  cfg.valveType = DEFAULT_VALVE_TYPE;
  cfg.valveServoPcaChannel = DEFAULT_VALVE_SERVO_CH;
  cfg.valveServoCloseAngle = 0;
  cfg.valveServoOpenAngle = 90;
  cfg.valveServoDir = 0;
  cfg.solenoidInterNoteMs = 0;
  cfg.motorType = DEFAULT_MOTOR_TYPE;
  cfg.fanPin = DEFAULT_FAN_PIN;
  cfg.fanMinPwm = DEFAULT_FAN_MIN_PWM;
  cfg.fanMaxPwm = DEFAULT_FAN_MAX_PWM;
  cfg.fanIdlePercent = DEFAULT_FAN_IDLE_PERCENT;
  cfg.fanIdleTimeoutMs = DEFAULT_FAN_IDLE_TIMEOUT_MS;
  cfg.fanDefaultPercent = DEFAULT_FAN_IDLE_PERCENT;
  cfg.fanMaxNotePercent = 100;
  cfg.fanFollowAirflow = true;
  cfg.numPumps = DEFAULT_NUM_PUMPS;
  cfg.pumpPins[0] = DEFAULT_PUMP_PIN;
  cfg.pumpPins[1] = 26;
  cfg.pumpPins[2] = 27;
  for (uint8_t i = 0; i < MAX_PUMPS; i++) {
    cfg.pumpMinPwm[i] = DEFAULT_PUMP_MIN_PWM;
    cfg.pumpMaxPwm[i] = DEFAULT_PUMP_MAX_PWM;
  }
  cfg.pumpCascadeThreshold = DEFAULT_PUMP_CASCADE_THRESHOLD;
  cfg.pumpStaggerMs = DEFAULT_PUMP_STAGGER_MS;
  cfg.pumpDirectIdlePercent = 0;
  cfg.pumpDirectMaxPercent = 100;
  cfg.pumpFollowAirflow = true;
  cfg.reservoirTargetPercent = 60;
  cfg.reservoirAutoStart = false;
  cfg.bangbangHysteresis = DEFAULT_BANGBANG_HYSTERESIS;
  cfg.sensorType = DEFAULT_SENSOR_TYPE;
  cfg.sensorTargetMm = DEFAULT_SENSOR_TARGET_MM;
  cfg.sensorMinMm = DEFAULT_SENSOR_MIN_MM;
  cfg.sensorMaxMm = DEFAULT_SENSOR_MAX_MM;
  cfg.pidKp = DEFAULT_PID_KP;
  cfg.pidKi = DEFAULT_PID_KI;
  cfg.endstopPin = DEFAULT_ENDSTOP_PIN;
  cfg.endstopActiveHigh = DEFAULT_ENDSTOP_ACTIVE_HIGH;
  cfg.endstopPumpOn = false;
  cfg.hallPin = DEFAULT_HALL_PIN;
  cfg.hallThresholdLow = DEFAULT_HALL_THRESHOLD_LOW;
  cfg.hallThresholdHigh = DEFAULT_HALL_THRESHOLD_HIGH;
  cfg.angleServoEnabled = false;
  cfg.angleServoPcaChannel = DEFAULT_ANGLE_SERVO_CH;
  cfg.showAirSystem = DEFAULT_SHOW_AIR_SYSTEM;
  strlcpy(cfg.resFormat, "balloon", sizeof(cfg.resFormat));

  // --- MIDI Storage ---
  cfg.midiStorageLimitKb = DEFAULT_MIDI_STORAGE_LIMIT_KB;

  // --- UI ---
  cfg.hideCalibration = false;
  cfg.hideAir = false;
  cfg.solenoidPin = SOLENOID_PIN;
  strncpy(cfg.instrumentColor, "#D4B044", sizeof(cfg.instrumentColor));
  cfg.kbdMode = 0;
}

bool ConfigStorage::load() {
  // D'abord initialiser les defauts
  initDefaults();

  File file = LittleFS.open(CONFIG_FILE_PATH, "r");
  if (!file) {
    if (DEBUG) {
      Serial.println("DEBUG: ConfigStorage - Pas de config sauvegardee, utilisation des defauts");
    }
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, file);
  file.close();

  if (err) {
    if (DEBUG) {
      Serial.print("ERREUR: ConfigStorage - JSON invalide: ");
      Serial.println(err.c_str());
    }
    return false;
  }

  // --- Instrument ---
  cfg.numFingers = doc["num_fingers"] | cfg.numFingers;
  if (cfg.numFingers < 1) cfg.numFingers = 1;
  if (cfg.numFingers > MAX_FINGER_SERVOS) cfg.numFingers = MAX_FINGER_SERVOS;

  cfg.numNotes = doc["num_notes"] | cfg.numNotes;
  if (cfg.numNotes < 1) cfg.numNotes = 1;
  if (cfg.numNotes > MAX_NOTES) cfg.numNotes = MAX_NOTES;

  cfg.airflowPcaChannel = doc["air_pca"] | cfg.airflowPcaChannel;
  cfg.fingerAngleOpen = doc["angle_open"] | cfg.fingerAngleOpen;
  cfg.halfHolePercent = doc["half_hole_pct"] | cfg.halfHolePercent;
  if (doc.containsKey("embouchure")) {
    strncpy(cfg.embouchure, doc["embouchure"] | "trav", sizeof(cfg.embouchure) - 1);
    cfg.embouchure[sizeof(cfg.embouchure) - 1] = '\0';
  }

  // --- Fingers ---
  JsonArray fingers = doc["fingers"];
  if (fingers) {
    for (int i = 0; i < cfg.numFingers && i < (int)fingers.size(); i++) {
      JsonObject f = fingers[i];
      cfg.fingers[i].pcaChannel = f["ch"] | cfg.fingers[i].pcaChannel;
      cfg.fingers[i].closedAngle = f["a"] | cfg.fingers[i].closedAngle;
      cfg.fingers[i].direction = f["d"] | cfg.fingers[i].direction;
      cfg.fingers[i].isThumbHole = f["th"] | (cfg.fingers[i].isThumbHole ? 1 : 0);
      cfg.fingers[i].halfPercent = f["hp"] | cfg.fingers[i].halfPercent;
    }
  }

  // --- Notes (complete: MIDI + finger patterns + airflow) ---
  JsonArray notes = doc["notes"];
  if (notes) {
    int count = min((int)notes.size(), (int)MAX_NOTES);
    cfg.numNotes = count;
    for (int i = 0; i < count; i++) {
      JsonObject n = notes[i];
      cfg.notes[i].midiNote = n["midi"] | cfg.notes[i].midiNote;
      cfg.notes[i].airflowMinPercent = n["amn"] | cfg.notes[i].airflowMinPercent;
      cfg.notes[i].airflowMaxPercent = n["amx"] | cfg.notes[i].airflowMaxPercent;
      cfg.notes[i].anglePercent = n["ang"] | cfg.notes[i].anglePercent;
      JsonArray fp = n["fp"];
      if (fp) {
        for (int f = 0; f < MAX_FINGER_SERVOS; f++) {
          cfg.notes[i].fingerPattern[f] = (f < (int)fp.size()) ? (uint8_t)fp[f].as<int>() : 0;
        }
      }
    }
  }

  // --- Scalaires (surcharge partielle, chaque champ optionnel) ---
  cfg.midiChannel = doc["midi_ch"] | cfg.midiChannel;
  cfg.serialMidiEnabled = doc["smidi_on"] | (cfg.serialMidiEnabled ? 1 : 0);
  cfg.serialMidiRxPin = doc["smidi_rx"] | cfg.serialMidiRxPin;
  cfg.servoToSolenoidDelayMs = doc["servo_delay"] | cfg.servoToSolenoidDelayMs;
  cfg.minNoteIntervalForValveCloseMs = doc["valve_interval"] | cfg.minNoteIntervalForValveCloseMs;
  cfg.minNoteDurationMs = doc["min_note_dur"] | cfg.minNoteDurationMs;
  cfg.servoAirflowOff = doc["air_off"] | cfg.servoAirflowOff;
  cfg.servoAirflowMin = doc["air_min"] | cfg.servoAirflowMin;
  cfg.servoAirflowMax = doc["air_max"] | cfg.servoAirflowMax;
  if (doc.containsKey("ang_pca")) cfg.angleServoPcaChannel = doc["ang_pca"] | cfg.angleServoPcaChannel;
  if (doc.containsKey("angle_ch")) cfg.angleServoPcaChannel = doc["angle_ch"] | cfg.angleServoPcaChannel;  // canonical field wins over legacy ang_pca
  cfg.servoAngleOff = doc["ang_off"] | cfg.servoAngleOff;
  cfg.servoAngleMin = doc["ang_min"] | cfg.servoAngleMin;
  cfg.servoAngleMax = doc["ang_max"] | cfg.servoAngleMax;
  cfg.vibratoFrequencyHz = doc["vib_freq"] | cfg.vibratoFrequencyHz;
  cfg.vibratoMaxAmplitudeDeg = doc["vib_amp"] | cfg.vibratoMaxAmplitudeDeg;
  cfg.ccVolumeDefault = doc["cc_vol"] | cfg.ccVolumeDefault;
  cfg.ccExpressionDefault = doc["cc_expr"] | cfg.ccExpressionDefault;
  cfg.ccModulationDefault = doc["cc_mod"] | cfg.ccModulationDefault;
  cfg.ccBreathDefault = doc["cc_breath"] | cfg.ccBreathDefault;
  cfg.ccBrightnessDefault = doc["cc_bright"] | cfg.ccBrightnessDefault;
  cfg.cc2Enabled = doc["cc2_on"] | (cfg.cc2Enabled ? 1 : 0);
  cfg.cc2SilenceThreshold = doc["cc2_thr"] | cfg.cc2SilenceThreshold;
  cfg.cc2ResponseCurve = doc["cc2_curve"] | cfg.cc2ResponseCurve;
  cfg.cc2TimeoutMs = doc["cc2_timeout"] | cfg.cc2TimeoutMs;
  cfg.solenoidPwmActivation = doc["sol_act"] | cfg.solenoidPwmActivation;
  cfg.solenoidPwmHolding = doc["sol_hold"] | cfg.solenoidPwmHolding;
  cfg.solenoidActivationTimeMs = doc["sol_time"] | cfg.solenoidActivationTimeMs;
  cfg.timeUnpower = doc["time_unpower"] | cfg.timeUnpower;
  cfg.hideCalibration = doc["hide_calib"] | (cfg.hideCalibration ? 1 : 0);
  cfg.hideAir = doc["hide_air"] | (cfg.hideAir ? 1 : 0);
  cfg.solenoidPin = doc["sol_pin"] | cfg.solenoidPin;
  cfg.kbdMode = doc["kbd_mode"] | cfg.kbdMode;
  const char* color = doc["color"];
  if (color) { strncpy(cfg.instrumentColor, color, sizeof(cfg.instrumentColor) - 1); cfg.instrumentColor[sizeof(cfg.instrumentColor) - 1] = '\0'; }
  cfg.airAttackMode = doc["air_atk_mode"] | cfg.airAttackMode;
  cfg.airAttackOffset = doc["air_atk_off"] | cfg.airAttackOffset;
  cfg.airAttackMs = doc["air_atk_ms"] | cfg.airAttackMs;
  cfg.airVelocityResponse = doc["air_vel_resp"] | cfg.airVelocityResponse;

  // --- Air delivery system (modulaire) ---
  cfg.airMode = doc["air_mode"] | cfg.airMode;
  // Retro-compat: ancien mode 6 (endstop) -> mode 5 avec sensorType=endstop meca
  if (cfg.airMode == 6) {
    cfg.airMode = AIR_MODE_PUMP_RESERVOIR;
    if (!doc.containsKey("sens_type")) cfg.sensorType = SENSOR_TYPE_ENDSTOP_MECH;
  }
  cfg.valveType = doc["valve_type"] | cfg.valveType;
  // Retro-compat: ancien champ valve_servo (bool) -> valveType
  if (doc.containsKey("valve_servo") && !doc.containsKey("valve_type")) {
    cfg.valveType = doc["valve_servo"].as<bool>() ? 1 : 0;
  }
  cfg.valveServoPcaChannel = doc["valve_ch"] | cfg.valveServoPcaChannel;
  cfg.valveServoCloseAngle = doc["vlv_close"] | cfg.valveServoCloseAngle;
  cfg.valveServoOpenAngle = doc["vlv_open"] | cfg.valveServoOpenAngle;
  // vlv_dir is legacy and intentionally ignored; close/open angles define movement.
  cfg.solenoidInterNoteMs = doc["sol_inter"] | cfg.solenoidInterNoteMs;
  cfg.motorType = doc["motor_type"] | cfg.motorType;
  cfg.fanPin = doc["fan_pin"] | cfg.fanPin;
  cfg.fanMinPwm = doc["fan_min"] | cfg.fanMinPwm;
  cfg.fanMaxPwm = doc["fan_max"] | cfg.fanMaxPwm;
  cfg.fanIdlePercent = doc["fan_idle_pct"] | cfg.fanIdlePercent;
  cfg.fanIdleTimeoutMs = doc["fan_idle_timeout"] | cfg.fanIdleTimeoutMs;
  cfg.fanDefaultPercent = doc["fan_default_pct"] | cfg.fanDefaultPercent;
  cfg.fanMaxNotePercent = doc["fan_note_max_pct"] | cfg.fanMaxNotePercent;
  cfg.fanFollowAirflow = doc["fan_follow_air"] | (cfg.fanFollowAirflow ? 1 : 0);
  cfg.numPumps = doc["num_pumps"] | cfg.numPumps;
  if (cfg.numPumps < 1) cfg.numPumps = 1;
  if (cfg.numPumps > MAX_PUMPS) cfg.numPumps = MAX_PUMPS;
  // Retro-compat: ancien champ pump_pin unique -> pumpPins[0]
  if (doc.containsKey("pump_pin") && !doc.containsKey("pump_pins")) {
    cfg.pumpPins[0] = doc["pump_pin"] | cfg.pumpPins[0];
    cfg.pumpMinPwm[0] = doc["pump_min"] | cfg.pumpMinPwm[0];
    cfg.pumpMaxPwm[0] = doc["pump_max"] | cfg.pumpMaxPwm[0];
  }
  JsonArray pumpPins = doc["pump_pins"];
  if (pumpPins) {
    for (int i = 0; i < MAX_PUMPS && i < (int)pumpPins.size(); i++) {
      cfg.pumpPins[i] = pumpPins[i] | cfg.pumpPins[i];
    }
  }
  JsonArray pumpMins = doc["pump_mins"];
  if (pumpMins) {
    for (int i = 0; i < MAX_PUMPS && i < (int)pumpMins.size(); i++) {
      cfg.pumpMinPwm[i] = pumpMins[i] | cfg.pumpMinPwm[i];
    }
  }
  JsonArray pumpMaxs = doc["pump_maxs"];
  if (pumpMaxs) {
    for (int i = 0; i < MAX_PUMPS && i < (int)pumpMaxs.size(); i++) {
      cfg.pumpMaxPwm[i] = pumpMaxs[i] | cfg.pumpMaxPwm[i];
    }
  }
  cfg.pumpCascadeThreshold = doc["pump_cascade"] | cfg.pumpCascadeThreshold;
  if (cfg.pumpCascadeThreshold > 100) cfg.pumpCascadeThreshold = 100;
  cfg.pumpStaggerMs = doc["pump_stagger"] | cfg.pumpStaggerMs;
  cfg.pumpDirectIdlePercent = doc["pump_idle_pct"] | cfg.pumpDirectIdlePercent;
  cfg.pumpDirectMaxPercent = doc["pump_direct_max_pct"] | cfg.pumpDirectMaxPercent;
  cfg.pumpFollowAirflow = doc["pump_follow_air"] | (cfg.pumpFollowAirflow ? 1 : 0);
  cfg.reservoirTargetPercent = doc["res_target_pct"] | cfg.reservoirTargetPercent;
  cfg.reservoirAutoStart = doc["res_autostart"] | (cfg.reservoirAutoStart ? 1 : 0);
  cfg.bangbangHysteresis = doc["bb_hyst"] | cfg.bangbangHysteresis;
  if (cfg.bangbangHysteresis > 50) cfg.bangbangHysteresis = 50;
  cfg.sensorType = doc["sens_type"] | cfg.sensorType;
  cfg.sensorTargetMm = doc["sens_target"] | cfg.sensorTargetMm;
  cfg.sensorMinMm = doc["sens_min"] | cfg.sensorMinMm;
  cfg.sensorMaxMm = doc["sens_max"] | cfg.sensorMaxMm;
  cfg.pidKp = doc["pid_kp"] | cfg.pidKp;
  cfg.pidKi = doc["pid_ki"] | cfg.pidKi;
  cfg.endstopPin = doc["endstop_pin"] | cfg.endstopPin;
  cfg.endstopActiveHigh = doc["endstop_high"] | (cfg.endstopActiveHigh ? 1 : 0);
  cfg.endstopPumpOn = doc["endstop_pump_on"] | (cfg.endstopPumpOn ? 1 : 0);
  cfg.hallPin = doc["hall_pin"] | cfg.hallPin;
  cfg.hallThresholdLow = doc["hall_low"] | cfg.hallThresholdLow;
  cfg.hallThresholdHigh = doc["hall_high"] | cfg.hallThresholdHigh;
  cfg.angleServoEnabled = doc["angle_on"] | (cfg.angleServoEnabled ? 1 : 0);
  cfg.angleServoPcaChannel = doc["angle_ch"] | cfg.angleServoPcaChannel;
  cfg.showAirSystem = doc["show_air"] | (cfg.showAirSystem ? 1 : 0);
  const char* rf = doc["res_format"];
  if (rf) { strlcpy(cfg.resFormat, rf, sizeof(cfg.resFormat)); }

  // --- MIDI Storage ---
  cfg.midiStorageLimitKb = doc["midi_limit"] | cfg.midiStorageLimitKb;

  const char* ssid = doc["wifi_ssid"];
  if (ssid) { strncpy(cfg.wifiSsid, ssid, sizeof(cfg.wifiSsid) - 1); cfg.wifiSsid[sizeof(cfg.wifiSsid) - 1] = '\0'; }

  const char* pass = doc["wifi_pass"];
  if (pass) { strncpy(cfg.wifiPassword, pass, sizeof(cfg.wifiPassword) - 1); cfg.wifiPassword[sizeof(cfg.wifiPassword) - 1] = '\0'; }

  const char* name = doc["device"];
  if (name) { strncpy(cfg.deviceName, name, sizeof(cfg.deviceName) - 1); cfg.deviceName[sizeof(cfg.deviceName) - 1] = '\0'; }

  ConfigValidationResult validation = validateAndNormalizeConfig(cfg);
  if (!validation.valid && DEBUG) {
    Serial.print("ERREUR: ConfigStorage - Config invalide normalisee/rejetee: ");
    Serial.println(validation.error);
  }

  if (DEBUG) {
    Serial.println("DEBUG: ConfigStorage - Config chargee depuis LittleFS");
    Serial.print("DEBUG:   Doigts: ");
    Serial.print(cfg.numFingers);
    Serial.print("  Notes: ");
    Serial.print(cfg.numNotes);
    Serial.print("  Airflow PCA: ");
    Serial.println(cfg.airflowPcaChannel);
  }

  return true;
}

bool ConfigStorage::save() {
  ConfigValidationResult validation = validateAndNormalizeConfig(cfg);
  if (!validation.valid) {
    if (DEBUG) { Serial.print("ERREUR: ConfigStorage - sauvegarde refusee: "); Serial.println(validation.error); }
    return false;
  }

  JsonDocument doc;

  // --- Instrument ---
  doc["num_fingers"] = cfg.numFingers;
  doc["num_notes"] = cfg.numNotes;
  doc["air_pca"] = cfg.airflowPcaChannel;
  doc["angle_open"] = cfg.fingerAngleOpen;
  doc["half_hole_pct"] = cfg.halfHolePercent;
  doc["embouchure"] = cfg.embouchure;

  // --- Fingers ---
  JsonArray fingers = doc["fingers"].to<JsonArray>();
  for (int i = 0; i < cfg.numFingers; i++) {
    JsonObject f = fingers.add<JsonObject>();
    f["ch"] = cfg.fingers[i].pcaChannel;
    f["a"] = cfg.fingers[i].closedAngle;
    f["d"] = cfg.fingers[i].direction;
    if (cfg.fingers[i].isThumbHole) {
      f["th"] = 1;
    }
    if (cfg.fingers[i].halfPercent > 0) {
      f["hp"] = cfg.fingers[i].halfPercent;
    }
  }

  // --- Notes (complete) ---
  JsonArray notes = doc["notes"].to<JsonArray>();
  for (int i = 0; i < cfg.numNotes; i++) {
    JsonObject n = notes.add<JsonObject>();
    n["midi"] = cfg.notes[i].midiNote;
    n["amn"] = cfg.notes[i].airflowMinPercent;
    n["amx"] = cfg.notes[i].airflowMaxPercent;
    n["ang"] = cfg.notes[i].anglePercent;
    JsonArray fp = n["fp"].to<JsonArray>();
    for (int f = 0; f < MAX_FINGER_SERVOS; f++) {
      fp.add((int)cfg.notes[i].fingerPattern[f]);
    }
  }

  // --- Scalaires ---
  doc["midi_ch"] = cfg.midiChannel;
  doc["smidi_on"] = cfg.serialMidiEnabled ? 1 : 0;
  doc["smidi_rx"] = cfg.serialMidiRxPin;
  doc["servo_delay"] = cfg.servoToSolenoidDelayMs;
  doc["valve_interval"] = cfg.minNoteIntervalForValveCloseMs;
  doc["min_note_dur"] = cfg.minNoteDurationMs;
  doc["air_off"] = cfg.servoAirflowOff;
  doc["air_min"] = cfg.servoAirflowMin;
  doc["air_max"] = cfg.servoAirflowMax;
  doc["angle_ch"] = cfg.angleServoPcaChannel;
  doc["ang_off"] = cfg.servoAngleOff;
  doc["ang_min"] = cfg.servoAngleMin;
  doc["ang_max"] = cfg.servoAngleMax;
  doc["vib_freq"] = cfg.vibratoFrequencyHz;
  doc["vib_amp"] = cfg.vibratoMaxAmplitudeDeg;
  doc["cc_vol"] = cfg.ccVolumeDefault;
  doc["cc_expr"] = cfg.ccExpressionDefault;
  doc["cc_mod"] = cfg.ccModulationDefault;
  doc["cc_breath"] = cfg.ccBreathDefault;
  doc["cc_bright"] = cfg.ccBrightnessDefault;
  doc["cc2_on"] = cfg.cc2Enabled ? 1 : 0;
  doc["cc2_thr"] = cfg.cc2SilenceThreshold;
  doc["cc2_curve"] = cfg.cc2ResponseCurve;
  doc["cc2_timeout"] = cfg.cc2TimeoutMs;
  doc["sol_act"] = cfg.solenoidPwmActivation;
  doc["sol_hold"] = cfg.solenoidPwmHolding;
  doc["sol_time"] = cfg.solenoidActivationTimeMs;
  doc["time_unpower"] = cfg.timeUnpower;
  doc["hide_calib"] = cfg.hideCalibration ? 1 : 0;
  doc["hide_air"] = cfg.hideAir ? 1 : 0;
  doc["sol_pin"] = cfg.solenoidPin;
  doc["kbd_mode"] = cfg.kbdMode;
  doc["color"] = cfg.instrumentColor;
  doc["air_atk_mode"] = cfg.airAttackMode;
  doc["air_atk_off"] = cfg.airAttackOffset;
  doc["air_atk_ms"] = cfg.airAttackMs;
  doc["air_vel_resp"] = cfg.airVelocityResponse;
  // --- Air delivery system (modulaire) ---
  doc["air_mode"] = cfg.airMode;
  doc["valve_type"] = cfg.valveType;
  doc["valve_ch"] = cfg.valveServoPcaChannel;
  doc["vlv_close"] = cfg.valveServoCloseAngle;
  doc["vlv_open"] = cfg.valveServoOpenAngle;
  doc["sol_inter"] = cfg.solenoidInterNoteMs;
  doc["motor_type"] = cfg.motorType;
  doc["fan_pin"] = cfg.fanPin;
  doc["fan_min"] = cfg.fanMinPwm;
  doc["fan_max"] = cfg.fanMaxPwm;
  doc["fan_idle_pct"] = cfg.fanIdlePercent;
  doc["fan_idle_timeout"] = cfg.fanIdleTimeoutMs;
  doc["fan_default_pct"] = cfg.fanDefaultPercent;
  doc["fan_note_max_pct"] = cfg.fanMaxNotePercent;
  doc["fan_follow_air"] = cfg.fanFollowAirflow ? 1 : 0;
  doc["num_pumps"] = cfg.numPumps;
  JsonArray pumpPins = doc["pump_pins"].to<JsonArray>();
  JsonArray pumpMins = doc["pump_mins"].to<JsonArray>();
  JsonArray pumpMaxs = doc["pump_maxs"].to<JsonArray>();
  for (int i = 0; i < MAX_PUMPS; i++) {
    pumpPins.add(cfg.pumpPins[i]);
    pumpMins.add(cfg.pumpMinPwm[i]);
    pumpMaxs.add(cfg.pumpMaxPwm[i]);
  }
  doc["pump_cascade"] = cfg.pumpCascadeThreshold;
  doc["pump_stagger"] = cfg.pumpStaggerMs;
  doc["pump_idle_pct"] = cfg.pumpDirectIdlePercent;
  doc["pump_direct_max_pct"] = cfg.pumpDirectMaxPercent;
  doc["pump_follow_air"] = cfg.pumpFollowAirflow ? 1 : 0;
  doc["res_target_pct"] = cfg.reservoirTargetPercent;
  doc["res_autostart"] = cfg.reservoirAutoStart ? 1 : 0;
  doc["bb_hyst"] = cfg.bangbangHysteresis;
  doc["sens_type"] = cfg.sensorType;
  doc["sens_target"] = cfg.sensorTargetMm;
  doc["sens_min"] = cfg.sensorMinMm;
  doc["sens_max"] = cfg.sensorMaxMm;
  doc["pid_kp"] = cfg.pidKp;
  doc["pid_ki"] = cfg.pidKi;
  doc["endstop_pin"] = cfg.endstopPin;
  doc["endstop_high"] = cfg.endstopActiveHigh ? 1 : 0;
  doc["endstop_pump_on"] = cfg.endstopPumpOn ? 1 : 0;
  doc["hall_pin"] = cfg.hallPin;
  doc["hall_low"] = cfg.hallThresholdLow;
  doc["hall_high"] = cfg.hallThresholdHigh;
  doc["angle_on"] = cfg.angleServoEnabled ? 1 : 0;
  doc["angle_ch"] = cfg.angleServoPcaChannel;
  doc["show_air"] = cfg.showAirSystem ? 1 : 0;
  doc["res_format"] = cfg.resFormat;
  doc["midi_limit"] = cfg.midiStorageLimitKb;
  doc["wifi_ssid"] = cfg.wifiSsid;
  doc["wifi_pass"] = cfg.wifiPassword;
  doc["device"] = cfg.deviceName;

  File file = LittleFS.open(CONFIG_FILE_PATH, "w");
  if (!file) {
    if (DEBUG) {
      Serial.println("ERREUR: ConfigStorage - Impossible d'ecrire le fichier config");
    }
    return false;
  }

  size_t written = serializeJson(doc, file);
  file.close();

  if (DEBUG) {
    Serial.print("DEBUG: ConfigStorage - Config sauvegardee (");
    Serial.print(written);
    Serial.println(" octets)");
  }

  return written > 0;
}

void ConfigStorage::resetToDefaults() {
  initDefaults();
  save();
  if (DEBUG) {
    Serial.println("DEBUG: ConfigStorage - Reset aux valeurs par defaut");
  }
}

void ConfigStorage::factoryReset() {
  initDefaults();
  // Supprimer le fichier config pour que isFirstBoot() retourne true
  // Le wizard le recreera via save() apres configuration
  LittleFS.remove(CONFIG_FILE_PATH);
  if (DEBUG) {
    Serial.println("DEBUG: ConfigStorage - Reset usine (fichier supprime)");
  }
}

bool ConfigStorage::isFirstBoot() {
  return !LittleFS.exists(CONFIG_FILE_PATH);
}

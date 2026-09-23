/***********************************************************************************************
 * ConfigDefaults - Fabrique de la configuration d'usine (aucune dependance flash)
 *
 * Ce fichier ne contient QUE le remplissage d'une RuntimeConfig avec les valeurs
 * par defaut de settings.h. Il n'inclut ni ArduinoJson ni LittleFS : c'est ce qui
 * le rend compilable et testable sur hote, la ou ConfigStorage.cpp ne l'est pas.
 ***********************************************************************************************/
#include "ConfigStorage.h"

void ConfigStorage::makeDefaultConfig(RuntimeConfig& out) {
  // Remise a zero d'ABORD : `out` est souvent un candidat fraichement alloue sur
  // le tas ou la pile. Sans cela, les octets de bourrage (padding) de la
  // structure et tout champ futur non affecte ici garderaient des restes, et la
  // configuration d'usine ne serait pas reproductible octet pour octet.
  memset(&out, 0, sizeof(out));

  // --- Instrument ---
  out.numFingers = DEFAULT_NUM_FINGERS;
  out.numNotes = DEFAULT_NUM_NOTES;
  out.airflowPcaChannel = DEFAULT_AIRFLOW_PCA_CHANNEL;
  out.fingerAngleOpen = ANGLE_OPEN;
  out.halfHolePercent = 50;
  strncpy(out.embouchure, "trav", sizeof(out.embouchure) - 1);
  out.embouchure[sizeof(out.embouchure) - 1] = '\0';

  // Zero-fill all arrays first
  memset(out.fingers, 0, sizeof(out.fingers));
  memset(out.notes, 0, sizeof(out.notes));

  // Load default finger configs
  for (int i = 0; i < DEFAULT_NUM_FINGERS && i < MAX_FINGER_SERVOS; i++) {
    out.fingers[i].pcaChannel = DEFAULT_FINGERS[i].pcaChannel;
    out.fingers[i].closedAngle = DEFAULT_FINGERS[i].closedAngle;
    out.fingers[i].direction = DEFAULT_FINGERS[i].direction;
    out.fingers[i].isThumbHole = DEFAULT_FINGERS[i].isThumbHole;
    out.fingers[i].halfPercent = 0;  // 0 = use global halfHolePercent
  }

  // Load default note configs
  for (int i = 0; i < DEFAULT_NUM_NOTES && i < MAX_NOTES; i++) {
    out.notes[i].midiNote = DEFAULT_NOTES[i].midiNote;
    out.notes[i].airflowMinPercent = DEFAULT_NOTES[i].airflowMinPercent;
    out.notes[i].airflowMaxPercent = DEFAULT_NOTES[i].airflowMaxPercent;
    out.notes[i].airflowNominalPercent = DEFAULT_NOTES[i].airflowNominalPercent;
    out.notes[i].anglePercent = DEFAULT_NOTES[i].anglePercent;
    // Copy finger pattern (default has DEFAULT_NUM_FINGERS, pad rest with 0)
    for (int f = 0; f < MAX_FINGER_SERVOS; f++) {
      out.notes[i].fingerPattern[f] = (f < DEFAULT_NUM_FINGERS) ? DEFAULT_NOTES[i].fingerPattern[f] : 0;
    }
  }

  // --- MIDI ---
  out.midiChannel = MIDI_CHANNEL;

  // --- Serial MIDI ---
  out.serialMidiEnabled = DEFAULT_SERIAL_MIDI_ENABLED;
  out.serialMidiRxPin = DEFAULT_SERIAL_MIDI_RX_PIN;

  // --- Timing ---
  out.servoToSolenoidDelayMs = SERVO_TO_SOLENOID_DELAY_MS;
  out.minNoteIntervalForValveCloseMs = MIN_NOTE_INTERVAL_FOR_VALVE_CLOSE_MS;
  out.minNoteDurationMs = MIN_NOTE_DURATION_MS;

  // --- Airflow ---
  out.servoAirflowOff = SERVO_AIRFLOW_OFF;
  out.servoAirflowMin = SERVO_AIRFLOW_MIN;
  out.servoAirflowMax = SERVO_AIRFLOW_MAX;

  // --- Angle servo (trav) ---
  out.servoAngleOff = SERVO_ANGLE_OFF;
  out.servoAngleMin = SERVO_ANGLE_MIN;
  out.servoAngleMax = SERVO_ANGLE_MAX;

  // --- Vibrato ---
  out.vibratoFrequencyHz = VIBRATO_FREQUENCY_HZ;
  out.vibratoMaxAmplitudeDeg = VIBRATO_MAX_AMPLITUDE_DEG;

  // --- CC defaults ---
  out.ccVolumeDefault = CC_VOLUME_DEFAULT;
  out.ccExpressionDefault = CC_EXPRESSION_DEFAULT;
  out.ccModulationDefault = CC_MODULATION_DEFAULT;
  out.ccBreathDefault = CC_BREATH_DEFAULT;
  out.ccBrightnessDefault = CC_BRIGHTNESS_DEFAULT;

  // --- CC2 ---
  out.cc2Enabled = CC2_ENABLED;
  out.cc2SilenceThreshold = CC2_SILENCE_THRESHOLD;
  out.cc2ResponseCurve = CC2_RESPONSE_CURVE;
  out.cc2TimeoutMs = CC2_TIMEOUT_MS;

  // --- Solenoide ---
  out.solenoidPwmActivation = SOLENOID_PWM_ACTIVATION;
  out.solenoidPwmHolding = SOLENOID_PWM_HOLDING;
  out.solenoidActivationTimeMs = SOLENOID_ACTIVATION_TIME_MS;

  // --- Expression airflow ---
  out.airAttackMode = 0;           // stable par defaut
  out.airAttackOffset = 20;        // 20% d'ecart
  out.airAttackMs = 150;           // 150ms transition
  out.airVelocityResponse = 50;    // 50% d'influence velocite

  // --- WiFi ---
  memset(out.wifiSsid, 0, sizeof(out.wifiSsid));
  memset(out.wifiPassword, 0, sizeof(out.wifiPassword));

  // --- Device ---
  strncpy(out.deviceName, DEVICE_NAME, sizeof(out.deviceName) - 1);
  out.deviceName[sizeof(out.deviceName) - 1] = '\0';

  // --- Power ---
  out.timeUnpower = TIMEUNPOWER;

  // --- Air delivery system (modulaire) ---
  out.airMode = DEFAULT_AIR_MODE;
  out.valveType = DEFAULT_VALVE_TYPE;
  out.valveServoPcaChannel = DEFAULT_VALVE_SERVO_CH;
  out.valveServoCloseAngle = 0;
  out.valveServoOpenAngle = 90;
  out.motorType = DEFAULT_MOTOR_TYPE;
  out.fanPin = DEFAULT_FAN_PIN;
  out.fanMinPwm = DEFAULT_FAN_MIN_PWM;
  out.fanMaxPwm = DEFAULT_FAN_MAX_PWM;
  out.fanIdlePercent = DEFAULT_FAN_IDLE_PERCENT;
  out.fanIdleTimeoutMs = DEFAULT_FAN_IDLE_TIMEOUT_MS;
  out.fanDefaultPercent = DEFAULT_FAN_IDLE_PERCENT;
  out.fanMaxNotePercent = 100;
  out.fanFollowAirflow = true;
  out.numPumps = DEFAULT_NUM_PUMPS;
  out.pumpPins[0] = DEFAULT_PUMP_PIN;
  out.pumpPins[1] = 26;
  out.pumpPins[2] = 27;
  for (uint8_t i = 0; i < MAX_PUMPS; i++) {
    out.pumpMinPwm[i] = DEFAULT_PUMP_MIN_PWM;
    out.pumpMaxPwm[i] = DEFAULT_PUMP_MAX_PWM;
  }
  out.pumpCascadeThreshold = DEFAULT_PUMP_CASCADE_THRESHOLD;
  out.pumpStaggerMs = DEFAULT_PUMP_STAGGER_MS;
  out.pumpDirectIdlePercent = 0;
  out.pumpDirectMaxPercent = 100;
  out.pumpFollowAirflow = true;
  out.reservoirTargetPercent = 60;
  out.reservoirAutoStart = false;
  out.bangbangHysteresis = DEFAULT_BANGBANG_HYSTERESIS;
  out.sensorType = DEFAULT_SENSOR_TYPE;
  out.sensorTargetMm = DEFAULT_SENSOR_TARGET_MM;
  out.sensorMinMm = DEFAULT_SENSOR_MIN_MM;
  out.sensorMaxMm = DEFAULT_SENSOR_MAX_MM;
  out.pidKp = DEFAULT_PID_KP;
  out.pidKi = DEFAULT_PID_KI;
  out.endstopPin = DEFAULT_ENDSTOP_PIN;
  out.endstopActiveHigh = DEFAULT_ENDSTOP_ACTIVE_HIGH;
  out.endstopPumpOn = false;
  out.hallPin = DEFAULT_HALL_PIN;
  out.hallThresholdLow = DEFAULT_HALL_THRESHOLD_LOW;
  out.hallThresholdHigh = DEFAULT_HALL_THRESHOLD_HIGH;
  out.angleServoEnabled = false;
  out.angleServoPcaChannel = DEFAULT_ANGLE_SERVO_CH;
  out.showAirSystem = DEFAULT_SHOW_AIR_SYSTEM;
  strncpy(out.resFormat, "balloon", sizeof(out.resFormat) - 1);
  out.resFormat[sizeof(out.resFormat) - 1] = '\0';

  // --- MIDI Storage ---
  out.midiStorageLimitKb = DEFAULT_MIDI_STORAGE_LIMIT_KB;

  // --- UI ---
  out.hideCalibration = false;
  out.hideAir = false;
  out.solenoidPin = SOLENOID_PIN;
  strncpy(out.instrumentColor, "#D4B044", sizeof(out.instrumentColor) - 1);
  out.instrumentColor[sizeof(out.instrumentColor) - 1] = '\0';
  out.kbdMode = 0;
}

// Chemin de BOOT uniquement : c'est la seule initialisation qui a le droit
// d'ecrire la configuration ACTIVE, parce qu'elle s'execute avant que quoi que ce
// soit ne pilote du materiel ou ne lise `cfg` en parallele. Les chemins de reset
// (ConfigStorage::resetToDefaults / factoryReset) passent par
// makeDefaultConfig(candidat) et laissent l'activation au redemarrage.
void ConfigStorage::initDefaults() {
  makeDefaultConfig(cfg);
}

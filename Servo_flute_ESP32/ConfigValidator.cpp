#include "ConfigStorage.h"
#include <math.h>

static void appendIssue(String& dst, const String& msg) {
  if (dst.length() > 0) dst += "; ";
  dst += msg;
}

static bool isReservedGpio(uint8_t pin) {
  return pin == STATUS_LED_PIN || pin == PAIRING_BUTTON_PIN || pin == MODE_SWITCH_PIN ||
         pin == I2C_SDA_PIN || pin == I2C_SCL_PIN || pin == PIN_SERVOS_OFF || pin == 1 || pin == 3;
}

// GPIO34-39 are input-only on the classic ESP32 and have NO internal pull-up.
static bool isInputOnlyGpio(uint8_t pin) {
  return pin >= 34 && pin <= 39;
}

// GPIO6-11 are wired to the on-package SPI flash: using them hangs the chip.
static bool isFlashGpio(uint8_t pin) {
  return pin >= 6 && pin <= 11;
}

// ESP32 strapping pins sampled at reset to set boot mode / flash voltage.
// Driving them from an actuator output can prevent boot or (GPIO12/MTDI, which
// selects the flash regulator voltage) brick a 3.3 V module by forcing 1.8 V.
// GPIO0/2/5 are already blocked as board-function pins; GPIO15 is otherwise only
// covered when the I2S mic is compiled in, so guard 12 and 15 explicitly here.
static bool isStrappingGpio(uint8_t pin) {
  return pin == 12 || pin == 15;
}

// Pins consumed by the INMP441 I2S microphone (only when the mic is compiled in).
static bool isI2sMicGpio(uint8_t pin) {
#if MIC_ENABLED
  return pin == MIC_PIN_BCLK || pin == MIC_PIN_LRCLK || pin == MIC_PIN_DIN;
#else
  (void)pin;
  return false;
#endif
}

// Reason a pin cannot be used at all (regardless of direction), or "" if usable.
// A single table so every configurable GPIO is validated the same way.
static const char* gpioHardConflict(uint8_t pin) {
  if (pin > CONFIG_MAX_GPIO) return "out of range";
  if (isFlashGpio(pin)) return "reserved for SPI flash";
  if (isReservedGpio(pin)) return "reserved by board function";
  if (isStrappingGpio(pin)) return "strapping pin (boot/flash-voltage select)";
  if (isI2sMicGpio(pin)) return "used by I2S microphone";
  return "";
}

// Un float venu du reseau (ou d'un /config.json corrompu) peut etre NaN ou Inf.
// Dans ce cas on retombe sur `fallback` : sans cela, NaN traverse toutes les
// comparaisons (NaN < lo et NaN > hi sont faux) et finit dans un calcul d'angle
// servo ou une periode de vibrato -> modulo par zero ou consigne aberrante.
static bool normalizeRangeFloat(float& v, float lo, float hi, float fallback) {
  float old = v;
  if (!isfinite(v)) {
    v = fallback;
  } else {
    if (v < lo) v = lo;
    if (v > hi) v = hi;
  }
  // Comparaison via memcmp-like : NaN != NaN, donc un remplacement de NaN compte
  // bien comme une correction.
  return !(old == v);
}

// Reduit une chaine de configuration a un ensemble ferme de valeurs connues.
static bool normalizeEnumString(char* value, size_t size, const char* const* allowed,
                                size_t allowedCount, const char* fallback) {
  for (size_t i = 0; i < allowedCount; i++) {
    if (strncmp(value, allowed[i], size) == 0) return false;
  }
  strncpy(value, fallback, size - 1);
  value[size - 1] = '\0';
  return true;
}

// Couleur "#RRGGBB" : tout autre contenu est remplace par la valeur par defaut.
static bool normalizeHexColor(char* value, size_t size, const char* fallback) {
  bool ok = (value[0] == '#');
  if (ok) {
    size_t len = strnlen(value, size);
    ok = (len == 7);
    for (size_t i = 1; ok && i < 7; i++) {
      char c = value[i];
      ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }
  }
  if (ok) return false;
  strncpy(value, fallback, size - 1);
  value[size - 1] = '\0';
  return true;
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
  // LA RETOMBEE THERMIQUE DE LA BOBINE DOIT RESTER UNE RETOMBEE.
  // Les deux champs etaient bornes chacun de son cote a 0..255, sans AUCUNE
  // contrainte entre eux. sol_hold = 255 etait donc accepte et persiste : la
  // bascule d'AirflowController::update() s'executait bien, mais ecrivait 255 -
  // le seul mecanisme anti-chauffe du firmware devenait un no-op et la bobine
  // restait a pleine tension toute la duree d'une note tenue, des ~13 s d'un
  // balayage de calibration, des 30 s d'une session de test. Aucun champ de
  // l'interface n'expose sol_hold, mais POST /api/config l'accepte : c'est ici
  // que cela se refuse, pas dans l'interface.
  // On CORRIGE au lieu de refuser : ce validateur tourne aussi sur la
  // configuration relue en flash au demarrage, et un refus y laisserait
  // l'ancienne valeur dangereuse en place au lieu de la ramener au plafond.
  {
    const uint16_t holdCeiling = (uint16_t)(((uint32_t)config.solenoidPwmActivation *
                                             SOLENOID_HOLD_MAX_PERCENT + 50) / 100);
    if (config.solenoidPwmHolding > holdCeiling) {
      config.solenoidPwmHolding = (uint8_t)holdCeiling;
      r.corrected = true;
      appendIssue(r.warnings, "solenoidPwmHolding capped to " + String(holdCeiling) +
                              " (max " + String(SOLENOID_HOLD_MAX_PERCENT) +
                              "% of solenoidPwmActivation)");
    }
  }
  r.corrected |= normalizeRangeU8(config.kbdMode, 0, 1);

  // --- Valeurs par defaut des CC (7 bits MIDI) ---
  r.corrected |= normalizeRangeU8(config.ccVolumeDefault, 0, MIDI_CC_MAX);
  r.corrected |= normalizeRangeU8(config.ccExpressionDefault, 0, MIDI_CC_MAX);
  r.corrected |= normalizeRangeU8(config.ccModulationDefault, 0, MIDI_CC_MAX);
  r.corrected |= normalizeRangeU8(config.ccBreathDefault, 0, MIDI_CC_MAX);
  r.corrected |= normalizeRangeU8(config.ccBrightnessDefault, 0, MIDI_CC_MAX);
  r.corrected |= normalizeRangeU8(config.cc2SilenceThreshold, 0, MIDI_CC_MAX);

  // --- Flottants : isfinite() + bornes musicales raisonnables ---
  // vibratoFrequencyHz alimente fastSin(), qui calcule period = 1000/f puis un
  // modulo : une frequence nulle, negative, NaN ou > 1000 Hz donnerait une
  // periode nulle (division par zero) ou un vibrato inaudible.
  r.corrected |= normalizeRangeFloat(config.vibratoFrequencyHz, CONFIG_MIN_VIBRATO_HZ,
                                     CONFIG_MAX_VIBRATO_HZ, VIBRATO_FREQUENCY_HZ);
  // Amplitude bornee : un vibrato de plus de CONFIG_MAX_VIBRATO_DEG degres
  // ferait sortir le servo de souffle de sa plage calibree.
  r.corrected |= normalizeRangeFloat(config.vibratoMaxAmplitudeDeg, 0.0f,
                                     CONFIG_MAX_VIBRATO_DEG, VIBRATO_MAX_AMPLITUDE_DEG);
  // cc2ResponseCurve est l'exposant d'un powf() : 0 rendrait la reponse constante
  // (toujours 1.0 -> souffle bloque au maximum) et un NaN propagerait un angle
  // servo invalide.
  r.corrected |= normalizeRangeFloat(config.cc2ResponseCurve, CONFIG_MIN_CC2_CURVE,
                                     CONFIG_MAX_CC2_CURVE, CC2_RESPONSE_CURVE);

  // --- Durees / timings (uint16_t) : bornes anti-delai absurde ---
  r.corrected |= normalizeRangeU16(config.minNoteIntervalForValveCloseMs, 0, CONFIG_MAX_INTERVAL_MS);
  // Le plus SERRE des deux plafonds s'applique. CONFIG_MAX_SOLENOID_PULSE_MS
  // (5000) bornait deja la valeur, mais 5 s de pleine tension a CHAQUE
  // ouverture de valve n'est pas une duree d'appel d'electrovanne : c'est une
  // duree de chauffe, dix fois la duree d'une croche a 120 BPM. Voir
  // SOLENOID_PULSE_MAX_MS dans settings.h pour le dimensionnement.
  {
    const uint16_t pulseCeiling = SOLENOID_PULSE_MAX_MS < CONFIG_MAX_SOLENOID_PULSE_MS
                                    ? (uint16_t)SOLENOID_PULSE_MAX_MS
                                    : (uint16_t)CONFIG_MAX_SOLENOID_PULSE_MS;
    r.corrected |= normalizeRangeU16(config.solenoidActivationTimeMs, 0, pulseCeiling);
  }
  r.corrected |= normalizeRangeU16(config.cc2TimeoutMs, 0, CONFIG_MAX_CC2_TIMEOUT_MS);
  r.corrected |= normalizeRangeU16(config.airAttackMs, CONFIG_MIN_ATTACK_MS, CONFIG_MAX_ATTACK_MS);
  r.corrected |= normalizeRangeU16(config.fanIdleTimeoutMs, 0, CONFIG_MAX_FAN_IDLE_TIMEOUT_MS);
  r.corrected |= normalizeRangeU16(config.pumpStaggerMs, 0, CONFIG_MAX_PUMP_STAGGER_MS);
  r.corrected |= normalizeRangeU16(config.timeUnpower, 0, CONFIG_MAX_UNPOWER_MS);
  // timeUnpower = 0 reste ACCEPTE, mais plus en silence.
  // managePower() lit 0 comme "servos alimentes en permanence", et c'est une
  // demande legitime sur certains montages : un tampon qui doit rester plaque
  // sur son trou, un palonnier lourd dont la recherche de position a chaque
  // remise sous tension fait un bruit audible. C'est aussi, et en meme temps,
  // la DERNIERE protection contre un calage : a 200 ms l'OE retombe et un servo
  // bloque est desalimente ; a 0 le calage est definitif.
  // L'interdire ne supprimerait pas le danger, seulement son nom : qui veut
  // tenir une position mettrait 60000, soit une minute de calage - autant dire
  // toujours, pour un servo qui chauffe deja. Ce qui reduit reellement le
  // risque est de borner les COMMANDES (voir SERVO_TEST_MARGIN_DEG) ; ce qui
  // reste a faire ici est de rendre le choix visible, et a tout client, pas
  // seulement au navigateur.
  if (config.timeUnpower == 0) {
    appendIssue(r.warnings, "timeUnpower=0 holds servo power permanently: "
                            "a stalled servo is never de-energised");
  }

  // --- Capteurs : bornes physiques + anti division par zero ---
  r.corrected |= normalizeRangeU16(config.sensorTargetMm, 0, CONFIG_MAX_SENSOR_MM);
  r.corrected |= normalizeRangeU16(config.sensorMinMm, 0, CONFIG_MAX_SENSOR_MM);
  r.corrected |= normalizeRangeU16(config.sensorMaxMm, 0, CONFIG_MAX_SENSOR_MM);
  r.corrected |= normalizeRangeU16(config.hallThresholdLow, 0, CONFIG_MAX_ADC_RAW);
  r.corrected |= normalizeRangeU16(config.hallThresholdHigh, 0, CONFIG_MAX_ADC_RAW);
  r.corrected |= normalizeRangeU16(config.midiStorageLimitKb, CONFIG_MIN_MIDI_LIMIT_KB, CONFIG_MAX_MIDI_LIMIT_KB);

  // --- Chaines libres : ramenees a un ensemble ferme / a un format sur ---
  {
    static const char* kEmbouchures[] = {"trav", "bec", "naf", "end", "oca"};
    r.corrected |= normalizeEnumString(config.embouchure, sizeof(config.embouchure),
                                       kEmbouchures, 5, "trav");
    static const char* kResFormats[] = {"balloon", "bellows"};
    r.corrected |= normalizeEnumString(config.resFormat, sizeof(config.resFormat),
                                       kResFormats, 2, "balloon");
    r.corrected |= normalizeHexColor(config.instrumentColor, sizeof(config.instrumentColor), "#D4B044");
  }

  if (config.servoAirflowMin >= config.servoAirflowMax) appendIssue(r.error, "servoAirflowMin must be < servoAirflowMax");
  // PWM min == max sur une pompe rendrait la plage nulle : la consigne ne
  // pourrait plus varier (toujours le minimum), ce qui n'est pas une erreur mais
  // merite d'etre signale. Le cas min > max reste une erreur (verifie plus bas).
  if (configurationUsesPumps(config)) {
    for (uint8_t i = 0; i < config.numPumps && i < MAX_PUMPS; i++) {
      if (config.pumpMinPwm[i] == config.pumpMaxPwm[i]) {
        appendIssue(r.warnings, "pump " + String(i) + " PWM range is a single point");
      }
    }
  }
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

  // Build the set of every GPIO the configuration consumes, tagging each one as an
  // output (solenoid/fan/pump: needs a real output pin) or a pull-up input
  // (endstop: pinMode(INPUT_PULLUP)). Then validate every pin against a single
  // capability table and cross-check for conflicts (including the I2S mic and the
  // serial-MIDI UART, which live outside the PCA but share the same GPIO space).
  const uint8_t kMaxGpios = 4 + MAX_PUMPS;    // solenoid + fan + sensor + serialMIDI + pumps
  uint8_t gpios[kMaxGpios]; bool gpioIsOutput[kMaxGpios]; bool gpioNeedsPullup[kMaxGpios];
  uint8_t gcount = 0;
  auto addGpio = [&](uint8_t pin, bool isOutput, bool needsPullup) {
    if (gcount < kMaxGpios) { gpios[gcount] = pin; gpioIsOutput[gcount] = isOutput; gpioNeedsPullup[gcount] = needsPullup; gcount++; }
  };
  if (configurationUsesSolenoidValve(config)) addGpio(config.solenoidPin, true, false);
  if (configurationUsesFan(config)) addGpio(config.fanPin, true, false);
  if (configurationUsesPumps(config)) for (uint8_t i = 0; i < config.numPumps && i < MAX_PUMPS; i++) addGpio(config.pumpPins[i], true, false);
  if (configurationUsesReservoirSensor(config) && config.sensorType == SENSOR_TYPE_HALL_KY024) addGpio(config.hallPin, false, false);
  if (configurationUsesReservoirSensor(config) && (config.sensorType == SENSOR_TYPE_ENDSTOP_MECH || config.sensorType == SENSOR_TYPE_ENDSTOP_OPT)) addGpio(config.endstopPin, false, true);
  // The serial-MIDI RX pin was previously never validated nor added to the used set.
  if (config.serialMidiEnabled) addGpio(config.serialMidiRxPin, false, false);

  for (uint8_t i = 0; i < gcount; i++) {
    const char* hard = gpioHardConflict(gpios[i]);
    if (hard[0] != '\0') appendIssue(r.error, "GPIO " + String(gpios[i]) + " " + hard);
    if (gpioIsOutput[i] && isInputOnlyGpio(gpios[i])) appendIssue(r.error, "input-only GPIO used as output: " + String(gpios[i]));
    // GPIO34-39 have no internal pull-up, so a pin needing INPUT_PULLUP (endstop)
    // there floats: reject it (the default endstop GPIO34 was exactly this trap).
    if (gpioNeedsPullup[i] && isInputOnlyGpio(gpios[i])) appendIssue(r.error, "GPIO " + String(gpios[i]) + " needs a pull-up (34-39 have none)");
    for (uint8_t j = i + 1; j < gcount; j++) if (gpios[i] == gpios[j]) appendIssue(r.error, "duplicate incompatible GPIO: " + String(gpios[i]));
  }

  if (previousConfig) {
    if (previousConfig->numFingers != config.numFingers || previousConfig->airflowPcaChannel != config.airflowPcaChannel ||
        previousConfig->valveServoPcaChannel != config.valveServoPcaChannel || previousConfig->angleServoPcaChannel != config.angleServoPcaChannel ||
        previousConfig->airMode != config.airMode || previousConfig->valveType != config.valveType || previousConfig->solenoidPin != config.solenoidPin ||
        previousConfig->fanPin != config.fanPin || previousConfig->numPumps != config.numPumps || previousConfig->motorType != config.motorType ||
        previousConfig->sensorType != config.sensorType || previousConfig->hallPin != config.hallPin || previousConfig->endstopPin != config.endstopPin ||
        previousConfig->serialMidiEnabled != config.serialMidiEnabled || previousConfig->serialMidiRxPin != config.serialMidiRxPin ||
        // angleServoEnabled feeds requiresSecondPca(): toggling it on for an angle
        // servo on channel >=16 needs the 2nd PCA9685 to be initialized at boot.
        // Without a restart the board is never brought up and the servo is dead.
        previousConfig->angleServoEnabled != config.angleServoEnabled) {
      r.restartRequired = true;
    }
    for (uint8_t i = 0; i < config.numFingers && i < previousConfig->numFingers; i++) if (previousConfig->fingers[i].pcaChannel != config.fingers[i].pcaChannel) r.restartRequired = true;
    for (uint8_t i = 0; i < config.numPumps && i < previousConfig->numPumps; i++) if (previousConfig->pumpPins[i] != config.pumpPins[i]) r.restartRequired = true;
  }

  r.valid = (r.error.length() == 0);
  return r;
}

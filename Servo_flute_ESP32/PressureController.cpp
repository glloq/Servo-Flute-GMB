#include "PressureController.h"
#include "ConfigStorage.h"
#include <Wire.h>

// Les acces bas niveau aux capteurs ToF (VL53L0X / VL6180X) vivent desormais
// dans TofSensor : identification du composant, initialisation complete, mesure
// single-shot non bloquante, et distinction explicite entre "present sur le bus",
// "initialise", "mesure valide" et "en erreur".

PressureController::PressureController()
  : _sensorDetected(false), _sensorPresence(SENSOR_PRESENCE_ABSENT), _sensorType(0),
    _distanceMm(0), _hallValue(0), _endstopActive(false), _fillPercent(0),
    _tofRangeStartTime(0), _measurementValid(false),
    _lastValidReadTime(0), _everMeasured(false), _tofErrorCount(0),
    _targetPercent(0), _currentPumpPwm(0),
    _enabled(true), _testPumpIndex(-1), _testPumpPercent(0), _testPumpStart(0),
    _runawayLatched(false), _pumpRunSince(0), _pumpRunRefFill(0), _runawayFill(0),
    _activePumpCount(0), _bangbangPumpOn(false),
    _pidIntegral(0),
    _lastPidTime(0), _lastReadTime(0) {
  for (uint8_t i = 0; i < MAX_PUMPS; i++) {
    _pumpActive[i] = false;
    _pumpActivateTime[i] = 0;
  }
}

bool PressureController::begin() {
  _sensorType = cfg.sensorType;
  // Un redemarrage repart d'un etat neuf : aucune mesure acceptee, aucun verrou.
  _everMeasured = false;
  _measurementValid = false;
  _lastValidReadTime = 0;
  clearPumpRunawayFault();

  // Configurer pins pompes
  if (cfg.airMode >= AIR_MODE_PUMP_VALVE) {
    for (uint8_t i = 0; i < cfg.numPumps && i < MAX_PUMPS; i++) {
      pinMode(cfg.pumpPins[i], OUTPUT);
      if (cfg.motorType == MOTOR_TYPE_ONOFF) {
        digitalWrite(cfg.pumpPins[i], LOW);
      } else {
        analogWrite(cfg.pumpPins[i], 0);
      }
    }
    if (DEBUG) {
      Serial.print("DEBUG: PressureController - Moteur ");
      Serial.println(cfg.motorType == MOTOR_TYPE_ONOFF ? "On/Off" : "PWM");
    }
  }

  // Mode pompe directe (sans reservoir) - pas de capteur a configurer
  if (cfg.airMode != AIR_MODE_PUMP_RESERVOIR) {
    if (DEBUG) {
      Serial.println("DEBUG: PressureController - Mode pompe directe (sans capteur)");
    }
    return false;
  }

  // Mode reservoir: configurer selon type de capteur
  if (_sensorType == SENSOR_TYPE_ENDSTOP_MECH || _sensorType == SENSOR_TYPE_ENDSTOP_OPT) {
    // Endstop mecanique ou optique: entree digitale.
    //
    // Le rappel interne suit la polarite DECLAREE et produit le niveau INACTIF :
    // un contact sec n'impose qu'un seul des deux niveaux, l'autre vient du
    // rappel. L'ancien INPUT_PULLUP inconditionnel contredisait le defaut du
    // firmware (DEFAULT_ENDSTOP_ACTIVE_HIGH = true) : sur un capteur actif-HIGH
    // il lisait "actif" en permanence, donc "reservoir plein" en permanence.
    //
    // Ce choix n'est PAS une securite et ne doit jamais etre pris pour telle :
    // une entree debranchee est electriquement identique a un contact relache,
    // aucune polarite ne les distingue. La securite qui couvre les deux est le
    // chien de garde de marche (PUMP_MAX_RUN_MS), qui ne regarde aucun drapeau.
    pinMode(cfg.endstopPin, endstopPinModeFor(cfg.endstopActiveHigh));
    // Une entree logique ne s'identifie pas : "presume", jamais "detecte".
    _sensorPresence = SENSOR_PRESENCE_PRESUMED;
    _sensorDetected = true;
    if (DEBUG) {
      Serial.print("DEBUG: PressureController - ");
      Serial.print(_sensorType == SENSOR_TYPE_ENDSTOP_MECH ? "Endstop mecanique" : "Endstop optique");
      Serial.print(" GPIO ");
      Serial.print(cfg.endstopPin);
      Serial.println(cfg.endstopActiveHigh ? " (actif HIGH)" : " (actif LOW)");
    }
    return true;
  }

  if (_sensorType == SENSOR_TYPE_HALL_KY024) {
    // Capteur effet Hall KY-024: entree analogique.
    //
    // Rien sur une entree ADC ne dit "KY-024" : il n'existe aucune identification
    // possible, donc l'etat honnete est PRESUME, jamais DETECTE. Ce qui est
    // verifiable, en revanche, c'est qu'une lecture est une MESURE : on sonde la
    // broche au demarrage et on refuse ce qui ne peut pas venir d'un capteur
    // ratiometrique alimente (rail bas = fil coupe/masse, rail haut = court-circuit).
    pinMode(cfg.hallPin, INPUT);
    _sensorPresence = SENSOR_PRESENCE_PRESUMED;
    _sensorDetected = true;
    bool plausible = false;
    unsigned long probeTime = millis();
    for (uint8_t i = 0; i < HALL_PROBE_SAMPLES && !plausible; i++) {
      plausible = acceptHallReading((uint16_t)analogRead(cfg.hallPin), probeTime);
    }
    if (plausible) _lastReadTime = probeTime;
    // Sonde negative : on ne declare pas le capteur absent pour autant (sur une
    // broche ADC2 une lecture nulle peut aussi vouloir dire "le WiFi a pris le
    // convertisseur", et un module peut s'alimenter apres le microcontroleur).
    // Mais AUCUNE mesure n'est acceptee : isMeasurementStale() reste vrai et la
    // pompe ne demarrera pas tant qu'une lecture plausible n'arrivera pas. C'est
    // la difference avec l'ancien code, qui posait _sensorDetected = true sans
    // rien sonder et laissait le PID pousser la pompe a 255 indefiniment.
    if (DEBUG) {
      Serial.print("DEBUG: PressureController - Hall KY-024 GPIO ");
      Serial.print(cfg.hallPin);
      Serial.print(" seuils ");
      Serial.print(cfg.hallThresholdLow);
      Serial.print("-");
      Serial.print(cfg.hallThresholdHigh);
      Serial.println(plausible ? " (presume present)" : " (AUCUNE lecture plausible : pompe bloquee)");
    }
    return true;
  }

  // Capteurs ToF (VL53L0X / VL6180X) : identification + initialisation complete.
  // Le simple fait qu'un composant acquitte a 0x29 ne suffit plus a declarer le
  // capteur fonctionnel : TofSensor verifie le Model ID puis deroule la sequence
  // d'initialisation (SPAD de reference, tuning, calibrations). Sans cela les
  // distances lues n'ont aucune signification metrique.
  bool initialized = _tof.begin(_sensorType == SENSOR_TYPE_TOF_VL6180X ? TOF_MODEL_VL6180X
                                                                       : TOF_MODEL_VL53L0X);
  _sensorDetected = initialized;
  // Seule famille reellement identifiable : le Model ID repond ou il ne repond pas.
  _sensorPresence = initialized ? SENSOR_PRESENCE_DETECTED : SENSOR_PRESENCE_ABSENT;
  if (!initialized && DEBUG) {
    Serial.print("ERREUR: PressureController - capteur ToF inutilisable (");
    Serial.print(_tof.stateName());
    Serial.println(") : la pompe restera coupee en mode reservoir");
  }
  return initialized;
}

bool PressureController::usesTofSensor() const {
  return _sensorType == SENSOR_TYPE_TOF_VL53L0X || _sensorType == SENSOR_TYPE_TOF_VL6180X;
}

bool PressureController::isMeasurementStale() const {
  // "Perimee" = aucune mesure ACCEPTEE depuis le delai de la famille de capteur,
  // que la derniere tentative ait echoue ou que le capteur se soit simplement fige.
  //
  // Cette garde ne depend PLUS du type de capteur. Elle commencait par
  // `if (!usesTofSensor()) return false;` : elle se declarait donc "pas perimee"
  // d'office sur un Hall ou une fin de course, c'est-a-dire qu'elle desactivait
  // la seule securite de mesure pour deux familles de capteurs sur trois.
  if (cfg.airMode != AIR_MODE_PUMP_RESERVOIR) return false;  // aucune mesure attendue
  // Rien n'a JAMAIS ete mesure : l'epoque (_lastValidReadTime = 0) n'est pas une
  // mesure. Sans ce cas, la pompe pouvait reguler pendant tout le premier delai
  // suivant le boot sur un capteur qui n'a jamais rien rendu.
  if (!_everMeasured) return true;
  if (usesTofSensor()) return (millis() - _lastValidReadTime) >= TOF_STALE_MS;
  return (millis() - _lastValidReadTime) >= RESERVOIR_STALE_MS;
}

const char* PressureController::sensorStateName() const {
  // "no_effect" : le capteur repond peut-etre, mais la pompe a tourne sans que
  // la mesure bouge. Distinguer ce cas d'un capteur absent evite d'envoyer
  // l'operateur changer un capteur alors que c'est une fuite ou une valve fermee.
  if (_runawayLatched) return "no_effect";
  if (usesTofSensor()) return _tof.stateName();
  switch (_sensorPresence) {
    // "presumed" : broche configuree et lecture acceptee, mais AUCUNE
    // identification possible - ne jamais afficher "detecte" pour ces familles.
    case SENSOR_PRESENCE_PRESUMED: return _everMeasured ? "presumed" : "presumed_no_reading";
    case SENSOR_PRESENCE_DETECTED: return "ready";
    default: return "absent";
  }
}

void PressureController::clearPumpRunawayFault() {
  _runawayLatched = false;
  _pumpRunSince = 0;
  _pumpRunRefFill = 0;
  _runawayFill = 0;
}

bool PressureController::isHallReadingPlausible(uint16_t raw) const {
  // Une bande degeneree n'est pas une echelle : tout se lirait "0 %", donc
  // "reservoir vide", donc pompe au maximum pour toujours. Sans mesure
  // interpretable, il n'y a pas de mesure.
  if (cfg.hallThresholdHigh <= cfg.hallThresholdLow) return false;
  // Entree collee a un rail : fil coupe / masse (bas) ou court-circuit a
  // l'alimentation (haut). La sortie d'un Hall lineaire alimente vit autour de
  // VCC/2 et ne peut pas s'y trouver ; en plus l'ADC de l'ESP32 n'est pas
  // lineaire dans ces quelques comptes. Ce n'est pas une mesure basse, c'est
  // une absence de mesure - et c'est precisement ce que l'ancien code lisait
  // comme "reservoir vide".
  if (raw <= HALL_RAW_STUCK_LOW || raw >= HALL_RAW_STUCK_HIGH) return false;
  return true;
}

bool PressureController::acceptHallReading(uint16_t raw, unsigned long now) {
  // Un SEUL endroit accepte une lecture Hall : le sondage de begin() et la
  // lecture periodique passent par ici. Deux chemins separes finiraient par
  // diverger - par exemple en horodatant une mesure "fraiche" sans mettre a jour
  // le remplissage, ce qui ferait reguler le PID sur un 0 % jamais mesure.
  _hallValue = raw;              // toujours expose au diagnostic, meme refuse
  if (!isHallReadingPlausible(raw)) {
    // Refusee : on ne remplace pas la mesure manquante par la derniere connue.
    // _lastValidReadTime n'avance pas -> peremption -> pompe coupee.
    _measurementValid = false;
    return false;
  }
  _measurementValid = true;
  _everMeasured = true;
  _lastValidReadTime = now;
  if (raw <= cfg.hallThresholdLow) {
    _fillPercent = 0;
  } else if (raw >= cfg.hallThresholdHigh) {
    _fillPercent = 100;
  } else {
    uint16_t hallSpan = cfg.hallThresholdHigh - cfg.hallThresholdLow;
    _fillPercent = (hallSpan == 0) ? 0 : (uint8_t)(((uint32_t)(raw - cfg.hallThresholdLow) * 100) / hallSpan);
  }
  return true;
}

void PressureController::serviceHallMeasurement(unsigned long now) {
  if (now - _lastReadTime < PRESSURE_READ_INTERVAL_MS) return;
  _lastReadTime = now;
  acceptHallReading((uint16_t)analogRead(cfg.hallPin), now);
}

void PressureController::serviceEndstopMeasurement(unsigned long now) {
  // Une entree logique ne peut pas etre "invraisemblable" : elle rend toujours un
  // niveau, et un fil coupe rend le niveau du rappel interne. La seule chose
  // verifiable ici est la FRAICHEUR : si la boucle principale se fige, la mesure
  // vieillit et la garde de peremption coupe la pompe au reveil. Que la valeur
  // lue soit vraie, seul le chien de garde de marche peut le contredire.
  _endstopActive = (digitalRead(cfg.endstopPin) == (cfg.endstopActiveHigh ? HIGH : LOW));
  _fillPercent = _endstopActive ? 100 : 0;
  _measurementValid = true;
  _everMeasured = true;
  _lastValidReadTime = now;
  _lastReadTime = now;
}

bool PressureController::fillProgressedFrom(uint8_t ref) const {
  // Sens attendu : la pompe remplit, sauf en mode "vidage" sur fin de course.
  const bool endstopFamily = (_sensorType == SENSOR_TYPE_ENDSTOP_MECH ||
                              _sensorType == SENSOR_TYPE_ENDSTOP_OPT);
  const int16_t fill = (int16_t)_fillPercent;
  const int16_t from = (int16_t)ref;
  if (endstopFamily && cfg.endstopPumpOn) return fill <= from - (int16_t)PUMP_PROGRESS_MIN_PERCENT;
  return fill >= from + (int16_t)PUMP_PROGRESS_MIN_PERCENT;
}

void PressureController::serviceRunawayWatchdog(unsigned long now) {
  // POURQUOI : une pompe qui tourne sans que la mesure bouge ne remplit rien.
  // Capteur mort lu comme "vide", fin de course figee dans le sens "remplir",
  // fuite, valve fermee : le code precedent n'avait AUCUNE limite de duree, donc
  // n'importe laquelle de ces pannes laissait la pompe a 255 indefiniment - sans
  // note, sans client web, depuis la seule mise sous tension. Cette fenetre est
  // la seule securite qui ne depende ni du type de capteur ni de sa polarite.
  if (_currentPumpPwm == 0) {
    _pumpRunSince = 0;
    // Rearmement : la mesure qui BOUGE alors que la pompe est arretee prouve que
    // le capteur vit encore (le reservoir se vide en jouant). C'est la seule
    // chose qui rearme automatiquement ; un capteur mort ne bouge jamais, donc
    // le verrou tient. Rearmer sur un simple arret rendrait la limite inutile :
    // la pompe repartirait pour une fenetre, indefiniment.
    if (_runawayLatched && _everMeasured) {
      int16_t moved = (int16_t)_fillPercent - (int16_t)_runawayFill;
      if (moved < 0) moved = -moved;
      if (moved >= (int16_t)PUMP_PROGRESS_MIN_PERCENT) clearPumpRunawayFault();
    }
    return;
  }
  if (_pumpRunSince == 0) {                 // la pompe vient de demarrer
    _pumpRunSince = now;
    _pumpRunRefFill = _fillPercent;
    return;
  }
  if (fillProgressedFrom(_pumpRunRefFill)) {
    // La pompe agit : on repart pour une fenetre depuis le nouveau niveau. La
    // reference est un cliquet (elle ne redescend pas) pour que le bruit de
    // mesure ne puisse pas relancer la fenetre indefiniment.
    _pumpRunSince = now;
    _pumpRunRefFill = _fillPercent;
    return;
  }
  if (now - _pumpRunSince >= PUMP_MAX_RUN_MS) {
    _runawayLatched = true;
    _runawayFill = _fillPercent;
  }
}

void PressureController::cutPumpForSafety() {
  setPumpPwm(0);
  _bangbangPumpOn = false;
  // L'integrateur accumule pendant que la mesure est douteuse : conserve, il se
  // dechargerait d'un coup a plein regime des le retour de la mesure.
  _pidIntegral = 0;
}

bool PressureController::serviceTofMeasurement() {
  if (!_sensorDetected || !_tof.isInitialized()) return false;
  unsigned long now = millis();

  if (!_tof.isRanging()) {
    // Start a new single-shot only at the configured read cadence.
    if (now - _lastReadTime < PRESSURE_READ_INTERVAL_MS) return false;
    _lastReadTime = now;
    if (!_tof.startMeasurement()) return false;
    _tofRangeStartTime = now;
    return false;
  }

  // Ranging in progress: poll the status register ONCE per call (no busy-wait,
  // so MIDI / WebSocket / audio / servos are not stalled up to 50 ms).
  if (_tof.pollMeasurement()) {
    _distanceMm = _tof.distanceMm();
    _measurementValid = true;
    _everMeasured = true;
    _lastValidReadTime = now;
    _tofErrorCount = 0;
    return true;
  }

  // Pas encore prete OU mesure rendue mais rejetee par le capteur : on abandonne
  // seulement au-dela du timeout. Un timeout ne lit jamais une valeur bidon ; la
  // mesure est marquee invalide et les echecs repetes invalident le capteur.
  if (!_tof.isRanging()) {
    // Le capteur a rendu une mesure invalide (range status != 11).
    _measurementValid = false;
    if (_tofErrorCount < 0xFFFF) _tofErrorCount++;
    if (_tofErrorCount >= TOF_MAX_CONSEC_ERRORS) {
      _tof.reportTimeout(1);
      _sensorDetected = false;
    }
    return false;
  }
  if (now - _tofRangeStartTime >= TOF_RANGE_TIMEOUT_MS) {
    _measurementValid = false;
    if (_tofErrorCount < 0xFFFF) _tofErrorCount++;
    _tof.abortMeasurement();
    if (_tofErrorCount >= TOF_MAX_CONSEC_ERRORS) {
      _tof.reportTimeout(1);
      _sensorDetected = false;
    }
  }
  return false;
}

void PressureController::update() {
  if (cfg.airMode < AIR_MODE_PUMP_VALVE) return;

  // Manual single-pump test overrides normal control: drive ONLY the tested pump
  // so a per-pump test never commands the others.
  if (_testPumpIndex >= 0) {
    // Un test manuel est BORNE DANS LE TEMPS. Il est lance par une commande web
    // et arrete par une autre : si l'onglet se ferme, si le WiFi tombe ou si
    // l'operateur s'en va, plus personne n'envoie l'arret et la pompe tourne
    // indefiniment. Un banc de test ne doit pas pouvoir devenir une panne.
    if (millis() - _testPumpStart >= PUMP_TEST_MAX_MS) {
      stopSinglePumpTest();
      return;
    }
    uint8_t raw = (uint16_t)_testPumpPercent * 255 / 100;
    _activePumpCount = 0;
    for (uint8_t i = 0; i < cfg.numPumps && i < MAX_PUMPS; i++) {
      bool on = (i == _testPumpIndex);
      writePumpHw(i, on ? raw : 0);
      _pumpActive[i] = (on && raw > 0);
      if (_pumpActive[i]) _activePumpCount++;
    }
    _currentPumpPwm = raw;
    return;
  }

  // Global mute: keep the pump off regardless of the requested target.
  if (!_enabled) {
    setPumpPwm(0);
    _bangbangPumpOn = false;
    return;
  }

  unsigned long now = millis();

  // Mode pompe directe (sans reservoir). setPumpPwm() accepts a logical raw demand
  // (0..255) and performs the physical min/max conversion exactly once.
  if (cfg.airMode != AIR_MODE_PUMP_RESERVOIR) {
    setPumpPwm((uint16_t)_targetPercent * 255 / 100);
    return;
  }

  // Reservoir mode must not silently degrade to direct mode if the sensor is absent.
  if (!_sensorDetected) {
    setPumpPwm(0);
    _bangbangPumpOn = false;
    _pidIntegral = 0;
    return;
  }

  // --- Mode reservoir avec capteur ---
  //
  // Trois etapes, dans cet ordre, pour TOUTES les familles de capteur :
  //   1. acquisition : une mesure n'est retenue que si elle est plausible ;
  //   2. gardes communes : peremption, emballement, cible nulle ;
  //   3. controle : bang-bang ou PID selon le type de moteur.
  // L'ORDRE EST LA CORRECTION : les branches Hall et fin de course rendaient la
  // main (return) AVANT les gardes, qui n'existaient que sur le chemin ToF. Un
  // capteur Hall debranche, lu ~0, passait donc pour "reservoir vide" et le PID
  // poussait la pompe a 255 pour toujours. Ne jamais remettre un `return` de
  // branche avant ce bloc de gardes.

  // 1) Acquisition (par famille)
  if (_sensorType == SENSOR_TYPE_ENDSTOP_MECH || _sensorType == SENSOR_TYPE_ENDSTOP_OPT) {
    serviceEndstopMeasurement(now);
  } else if (_sensorType == SENSOR_TYPE_HALL_KY024) {
    serviceHallMeasurement(now);
  } else if (serviceTofMeasurement()) {
    // ToF (VL53L0X / VL6180X): lecture I2C NON bloquante. _fillPercent n'est mis
    // a jour que sur une mesure fraiche et valide (jamais sur un timeout).
    if (_distanceMm <= cfg.sensorMinMm) {
      _fillPercent = 100;
    } else if (_distanceMm >= cfg.sensorMaxMm) {
      _fillPercent = 0;
    } else {
      uint16_t sensorSpan = cfg.sensorMaxMm - cfg.sensorMinMm;
      _fillPercent = (sensorSpan == 0) ? 0 : 100 - (uint8_t)(((uint32_t)(_distanceMm - cfg.sensorMinMm) * 100) / sensorSpan);
    }
  }

  // 2) Gardes communes A TOUTES les familles de capteur
  serviceRunawayWatchdog(now);

  // Securite mesure perimee (§20): sans mesure valide recente, on ne peut plus
  // reguler en securite -> couper la pompe plutot que de piloter sur une donnee
  // obsolete (un timeout lu comme distance 0 aurait sinon fait croire au reservoir plein).
  // Le critere est l'age de la DERNIERE mesure acceptee : un capteur fige qui ne
  // rend plus rien laisserait sinon la pompe reguler indefiniment sur l'avant-derniere.
  if (isMeasurementStale()) {
    cutPumpForSafety();
    return;
  }

  // Emballement : la pompe a tourne sans effet mesurable. On ne repart pas tout
  // seul, sinon la limite de duree ne serait qu'un rapport cyclique.
  if (_runawayLatched) {
    cutPumpForSafety();
    return;
  }

  if (_targetPercent == 0) {
    setPumpPwm(0);
    _bangbangPumpOn = false;
    _pidIntegral = 0;
    return;
  }

  // 3) Controle (par famille)

  // Endstop (mecanique ou optique): controle ON/OFF simple
  // endstopPumpOn: false = pompe ON quand capteur inactif (remplir), true = pompe ON quand capteur actif (vider)
  if (_sensorType == SENSOR_TYPE_ENDSTOP_MECH || _sensorType == SENSOR_TYPE_ENDSTOP_OPT) {
    bool shouldPump = cfg.endstopPumpOn ? _endstopActive : !_endstopActive;
    if (!shouldPump) {
      setPumpPwm(0);
      _bangbangPumpOn = false;
    } else {
      _bangbangPumpOn = true;
      setPumpPwm(255);
    }
    return;
  }

  // Hall effect: controle selon type moteur (la lecture a eu lieu en 1)
  if (_sensorType == SENSOR_TYPE_HALL_KY024) {
    if (now - _lastPidTime >= PRESSURE_PID_INTERVAL_MS) {
      _lastPidTime = now;

      if (cfg.motorType == MOTOR_TYPE_ONOFF) {
        // Bang-bang avec hysteresis pour moteurs On/Off
        uint8_t hyst = cfg.bangbangHysteresis;
        uint8_t lower = (_targetPercent > hyst) ? _targetPercent - hyst : 0;
        uint8_t upper = (_targetPercent + hyst < 100) ? _targetPercent + hyst : 100;
        if (_fillPercent < lower) {
          _bangbangPumpOn = true;
        } else if (_fillPercent > upper) {
          _bangbangPumpOn = false;
        }
        // Multi-pompe On/Off : graduer la demande selon l'ecart
        if (_bangbangPumpOn && cfg.numPumps > 1 && cfg.pumpCascadeThreshold > 0) {
          int16_t error = (int16_t)_targetPercent - (int16_t)_fillPercent;
          // Grand ecart (> 3x hysteresis) : toutes les pompes (cascade)
          // Petit ecart : pompe principale seule (sous le seuil cascade)
          setPumpPwm(error > (int16_t)(hyst * 3) ? 255 : 128);
        } else {
          setPumpPwm(_bangbangPumpOn ? 255 : 0);
        }
      } else {
        // PID pour moteurs PWM
        float error = (float)_targetPercent - (float)_fillPercent;
        float dt = PRESSURE_PID_INTERVAL_MS / 1000.0f;
        float kp = cfg.pidKp / 10.0f;
        float ki = cfg.pidKi / 10.0f;
        _pidIntegral += error * dt;
        if (_pidIntegral > 100.0f) _pidIntegral = 100.0f;
        if (_pidIntegral < -100.0f) _pidIntegral = -100.0f;
        float output = kp * error + ki * _pidIntegral;
        if (output <= 0) {
          setPumpPwm(0);
        } else {
          uint8_t pwm = (uint8_t)constrain(output * 2.55f, 0, 255);
          setPumpPwm(pwm);
        }
      }
    }
    return;
  }

  // ToF (VL53L0X / VL6180X): controle pompe selon type moteur (la mesure et les
  // gardes communes ont eu lieu plus haut).
  if (now - _lastPidTime >= PRESSURE_PID_INTERVAL_MS) {
    _lastPidTime = now;

    // Securite : distance > 300mm = capteur hors portee
    if (_distanceMm > PUMP_SAFETY_MAX_DIST_MM) {
      setPumpPwm(0);
      return;
    }

    // Securite : distance trop courte (surgonflage)
    if (_distanceMm <= PUMP_SAFETY_MIN_MM && _distanceMm > 0) {
      setPumpPwm(0);
      _bangbangPumpOn = false;
      return;
    }

    if (cfg.motorType == MOTOR_TYPE_ONOFF) {
      // Bang-bang avec hysteresis pour moteurs On/Off
      uint8_t hyst = cfg.bangbangHysteresis;
      uint8_t lower = (_targetPercent > hyst) ? _targetPercent - hyst : 0;
      uint8_t upper = (_targetPercent + hyst < 100) ? _targetPercent + hyst : 100;
      if (_fillPercent < lower) {
        _bangbangPumpOn = true;
      } else if (_fillPercent > upper) {
        _bangbangPumpOn = false;
      }
      setPumpPwm(_bangbangPumpOn ? 255 : 0);
    } else {
      // PID pour moteurs PWM
      float error = (float)_targetPercent - (float)_fillPercent;
      float dt = PRESSURE_PID_INTERVAL_MS / 1000.0f;
      float kp = cfg.pidKp / 10.0f;
      float ki = cfg.pidKi / 10.0f;
      _pidIntegral += error * dt;
      if (_pidIntegral > 100.0f) _pidIntegral = 100.0f;
      if (_pidIntegral < -100.0f) _pidIntegral = -100.0f;
      float output = kp * error + ki * _pidIntegral;
      if (output <= 0) {
        setPumpPwm(0);
      } else {
        uint8_t pwm = (uint8_t)constrain(output * 2.55f, 0, 255);
        setPumpPwm(pwm);
      }
    }
  }
}

void PressureController::setTargetPercent(uint8_t percent) {
  if (percent > 100) percent = 100;
  _targetPercent = percent;
}

void PressureController::stop() {
  _targetPercent = 0;
  _testPumpIndex = -1;   // a full stop also ends any single-pump test
  setPumpPwm(0);
  _pidIntegral = 0;
  _bangbangPumpOn = false;
  for (uint8_t i = 0; i < MAX_PUMPS; i++) {
    _pumpActive[i] = false;
    _pumpActivateTime[i] = 0;
  }
  _activePumpCount = 0;
}

void PressureController::setEnabled(bool enabled) {
  _enabled = enabled;
  // Cut the output now (don't wait for the next update). Keep _targetPercent so
  // unmuting resumes the previous demand rather than requiring a fresh note.
  if (!enabled) { setPumpPwm(0); _bangbangPumpOn = false; }
}

void PressureController::testSinglePump(uint8_t index, uint8_t percent) {
  if (index >= cfg.numPumps || index >= MAX_PUMPS) return;
  if (percent > 100) percent = 100;
  _testPumpIndex = (int8_t)index;
  _testPumpPercent = percent;
  // Depart du compte a rebours du test : un ordre repete le prolonge, l'absence
  // d'ordre l'arrete (cf. PUMP_TEST_MAX_MS dans update()).
  _testPumpStart = millis();
}

void PressureController::stopSinglePumpTest() {
  _testPumpIndex = -1;
  _testPumpStart = 0;
  setPumpPwm(0);
}

void PressureController::writePumpHw(uint8_t index, uint8_t pwm) {
  if (index >= cfg.numPumps || index >= MAX_PUMPS) return;
  if (cfg.motorType == MOTOR_TYPE_ONOFF) {
    digitalWrite(cfg.pumpPins[index], pwm > 0 ? HIGH : LOW);
  } else {
    uint8_t pumpVal = 0;
    if (pwm > 0) {
      pumpVal = cfg.pumpMinPwm[index] + (uint16_t)(cfg.pumpMaxPwm[index] - cfg.pumpMinPwm[index]) * pwm / 255;
      if (pumpVal < cfg.pumpMinPwm[index]) pumpVal = cfg.pumpMinPwm[index];
    }
    analogWrite(cfg.pumpPins[index], pumpVal);
  }
}

void PressureController::setPumpPwm(uint8_t pwm) {
  _currentPumpPwm = pwm;
  if (cfg.airMode < AIR_MODE_PUMP_VALVE) return;

  unsigned long now = millis();

  // === Mode parallele (1 pompe ou cascade desactivee) ===
  if (cfg.numPumps <= 1 || cfg.pumpCascadeThreshold == 0) {
    _activePumpCount = 0;
    for (uint8_t i = 0; i < cfg.numPumps && i < MAX_PUMPS; i++) {
      writePumpHw(i, pwm);
      _pumpActive[i] = (pwm > 0);
      if (pwm > 0) _activePumpCount++;
    }
    return;
  }

  // === Mode cascade : pompe 0 toujours active, les suivantes au seuil ===
  uint8_t cascade = cfg.pumpCascadeThreshold > 99 ? 99 : cfg.pumpCascadeThreshold;
  uint8_t threshPwm = (uint16_t)cascade * 255 / 100;
  _activePumpCount = 0;

  for (uint8_t i = 0; i < cfg.numPumps && i < MAX_PUMPS; i++) {
    if (i == 0) {
      // Pompe principale : toujours proportionnelle a la demande
      writePumpHw(0, pwm);
      _pumpActive[0] = (pwm > 0);
      if (pwm > 0) {
        _activePumpCount++;
        if (!_pumpActivateTime[0]) _pumpActivateTime[0] = now;
      } else {
        _pumpActivateTime[0] = 0;
      }
    } else {
      // Pompes secondaires : cascade
      if (pwm >= threshPwm && pwm > 0) {
        // Demande depasse le seuil -> pompe i doit s'activer
        if (!_pumpActive[i]) {
          _pumpActive[i] = true;
          _pumpActivateTime[i] = now;
        }
        // Delai stagger : attendre avant de demarrer
        unsigned long staggerDelay = (unsigned long)i * cfg.pumpStaggerMs;
        if (now - _pumpActivateTime[i] >= staggerDelay) {
          // PWM proportionnel au depassement du seuil
          uint8_t overflow = pwm - threshPwm;
          uint8_t denom = 255 - threshPwm;
          uint8_t pumpPwm = (denom == 0) ? 255 : (uint8_t)((uint16_t)overflow * 255 / denom);
          writePumpHw(i, pumpPwm);
          _activePumpCount++;
        } else {
          writePumpHw(i, 0); // En attente stagger
        }
      } else {
        // Sous le seuil : pompe i arretee
        _pumpActive[i] = false;
        _pumpActivateTime[i] = 0;
        writePumpHw(i, 0);
      }
    }
  }
}

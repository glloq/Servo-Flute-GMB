#include "InstrumentManager.h"
#include "ConfigStorage.h"

InstrumentManager::InstrumentManager()
  : _pwm0(PCA_ADDR_BOARD0),
    _pwm1(PCA_ADDR_BOARD1),
    _secondBoardEnabled(false),
    _eventQueue(EVENT_QUEUE_SIZE),
    _fingerCtrl([this](uint8_t ch, uint16_t on, uint16_t off) { setPWM(ch, on, off); }),
    _airflowCtrl([this](uint8_t ch, uint16_t on, uint16_t off) { setPWM(ch, on, off); }),
    _sequencer(_eventQueue, _fingerCtrl, _airflowCtrl),
    _lastActivityTime(0),
    _servosPowered(false),
    _ccVolume(cfg.ccVolumeDefault),
    _ccExpression(cfg.ccExpressionDefault),
    _ccModulation(cfg.ccModulationDefault),
    _ccBreath(cfg.ccBreathDefault),
    _ccBrightness(cfg.ccBrightnessDefault),
    _lastCCTime(0),
    _ccCount(0),
    _ccWindowStart(0),
    _cc2Count(0),
    _cc2WindowStart(0),
    _prevSequencerState(STATE_IDLE) {
}

void InstrumentManager::begin() {
  beginSafe();
}

bool InstrumentManager::beginSafe() {
  if (DEBUG) {
    Serial.println("DEBUG: InstrumentManager - Initialisation sure");
  }

  pinMode(PIN_SERVOS_OFF, OUTPUT);
  digitalWrite(PIN_SERVOS_OFF, HIGH);
  _servosPowered = false;

  _pwm0.begin();
  _pwm0.setPWMFreq(SERVO_FREQUENCY);
  delay(PWM_INIT_DELAY_MS);

  _secondBoardEnabled = false;
  for (int i = 0; i < cfg.numFingers; i++) {
    if (cfg.fingers[i].pcaChannel >= 16) {
      _secondBoardEnabled = true;
      break;
    }
  }
  if (cfg.airflowPcaChannel >= 16 ||
      (modeUsesPhysicalValve(cfg.airMode) && cfg.valveType == 1 && cfg.valveServoPcaChannel >= 16) ||
      (cfg.angleServoEnabled && cfg.angleServoPcaChannel >= 16)) {
    _secondBoardEnabled = true;
  }

  if (_secondBoardEnabled) {
    _pwm1.begin();
    _pwm1.setPWMFreq(SERVO_FREQUENCY);
    delay(PWM_INIT_DELAY_MS);
  }

  _fingerCtrl.begin();
  _airflowCtrl.begin();
  initializeSafeOutputs();

  if (cfg.airMode >= AIR_MODE_PUMP_VALVE) {
    bool sensorOk = _pressureCtrl.begin();
    _pressureCtrl.stop();
    if (DEBUG) {
      Serial.print("DEBUG: InstrumentManager - PressureController: ");
      Serial.println(sensorOk ? "capteur OK" : "sans capteur");
    }
  }

  if (cfg.airMode == AIR_MODE_FAN_SERVO) {
    _fanCtrl.begin();
    _fanCtrl.stop();
  }

  _airflowCtrl.setCCValues(_ccVolume, _ccExpression, _ccModulation);
  _sequencer.begin();
  _lastActivityTime = millis();

  powerOnServos();
  if (DEBUG) Serial.println("DEBUG: InstrumentManager - Pret (OE active apres sorties sures)");
  return true;
}

void InstrumentManager::initializeSafeOutputs() {
  _fingerCtrl.closeAllFingers();
  _airflowCtrl.closeValve();
  _airflowCtrl.setAirflowToRest();
  if (cfg.angleServoEnabled) _airflowCtrl.setAngleToRest();
}

void InstrumentManager::update() {
  _sequencer.update();
  _airflowCtrl.update();
  if (cfg.airMode >= AIR_MODE_PUMP_VALVE) {
    _pressureCtrl.update();
  }
  if (cfg.airMode == AIR_MODE_FAN_SERVO) {
    // Detect sequencer state transitions for fan idle management
    NoteState curState = _sequencer.getState();
    if (curState != _prevSequencerState) {
      if (curState == STATE_PLAYING && _prevSequencerState != STATE_PLAYING) {
        // Transition vers jeu actif => signaler noteOn au ventilateur
        _fanCtrl.onNoteOn();
      } else if (_prevSequencerState == STATE_PLAYING && curState != STATE_PLAYING) {
        // Transition depuis jeu actif => signaler noteOff au ventilateur
        _fanCtrl.onNoteOff();
      }
      _prevSequencerState = curState;
    }
    _fanCtrl.update();
  }
  managePower();
}

void InstrumentManager::noteOn(byte midiNote, byte velocity) {
  if (!isNotePlayable(midiNote)) {
    if (DEBUG) {
      Serial.print("DEBUG: InstrumentManager - Note hors plage: ");
      Serial.println(midiNote);
    }
    return;
  }

  if (cfg.airMode == AIR_MODE_FAN_SERVO) {
    uint8_t fanDemand = cfg.fanFollowAirflow ? (uint8_t)((uint16_t)cfg.fanMaxNotePercent * velocity / 127) : cfg.fanMaxNotePercent;
    if (fanDemand < cfg.fanDefaultPercent) fanDemand = cfg.fanDefaultPercent;
    _fanCtrl.setSpeed(fanDemand);
  }
  if (cfg.airMode == AIR_MODE_PUMP_VALVE) {
    uint8_t pumpDemand = cfg.pumpFollowAirflow ? (uint8_t)((uint16_t)cfg.pumpDirectMaxPercent * velocity / 127) : cfg.pumpDirectMaxPercent;
    if (pumpDemand < cfg.pumpDirectIdlePercent) pumpDemand = cfg.pumpDirectIdlePercent;
    _pressureCtrl.setTargetPercent(pumpDemand);
  }

  bool success = _eventQueue.enqueueLiveEvent(EVENT_NOTE_ON, midiNote, velocity);

  if (!success) {
    if (DEBUG) {
      Serial.println("ERREUR: InstrumentManager - Queue pleine!");
    }
  } else {
    if (DEBUG) {
      Serial.print("DEBUG: InstrumentManager - Note On: ");
      Serial.print(midiNote);
      Serial.print(" (vel: ");
      Serial.print(velocity);
      Serial.println(")");
    }
  }

  _lastActivityTime = millis();
}

void InstrumentManager::noteOff(byte midiNote) {
  if (cfg.airMode == AIR_MODE_PUMP_VALVE) {
    _pressureCtrl.setTargetPercent(cfg.pumpDirectIdlePercent);
  }

  bool success = _eventQueue.enqueueLiveEvent(EVENT_NOTE_OFF, midiNote, 0);

  if (!success) {
    if (DEBUG) {
      Serial.println("ERREUR: InstrumentManager - Queue pleine!");
    }
  } else {
    if (DEBUG) {
      Serial.print("DEBUG: InstrumentManager - Note Off: ");
      Serial.println(midiNote);
    }
  }

  _lastActivityTime = millis();
}

bool InstrumentManager::isNotePlayable(byte midiNote) const {
  return (getNoteByMidi(midiNote) != nullptr);
}

NoteSequencer& InstrumentManager::getSequencer() {
  return _sequencer;
}

void InstrumentManager::managePower() {
  if (_sequencer.isPlaying() || _sequencer.getState() != STATE_IDLE) {
    if (!_servosPowered) {
      powerOnServos();
    }
    _lastActivityTime = millis();
  } else {
    unsigned long elapsed = millis() - _lastActivityTime;
    if (elapsed >= cfg.timeUnpower && _servosPowered) {
      powerOffServos();
    }
  }
}

void InstrumentManager::powerOnServos() {
  digitalWrite(PIN_SERVOS_OFF, LOW);  // OE a LOW = servos actives
  _servosPowered = true;

  if (DEBUG) {
    Serial.println("DEBUG: InstrumentManager - Servos ACTIVES");
  }
}

void InstrumentManager::powerOffServos() {
  digitalWrite(PIN_SERVOS_OFF, HIGH);  // OE a HIGH = servos desactives
  _servosPowered = false;

  if (DEBUG) {
    Serial.println("DEBUG: InstrumentManager - Servos DESACTIVES (anti-bruit)");
  }
}

void InstrumentManager::handleControlChange(byte ccNumber, byte ccValue) {
  if (ccValue > MIDI_CC_MAX) {
    if (DEBUG) {
      Serial.print("ERREUR: CC invalide - valeur: ");
      Serial.println(ccValue);
    }
    return;
  }

  unsigned long currentTime = millis();

  // Rate limiting CC2 (Breath Controller) separe
  if (ccNumber == MIDI_CC_BREATH) {
    if (cfg.cc2Enabled) {
      if (currentTime - _cc2WindowStart >= CC_RATE_WINDOW_MS) {
        _cc2WindowStart = currentTime;
        _cc2Count = 0;
      }
      _cc2Count++;
      if (_cc2Count > CC2_RATE_LIMIT_PER_SECOND) {
        return;
      }
    }
  } else {
    // Rate limiting normal
    if (currentTime - _ccWindowStart >= CC_RATE_WINDOW_MS) {
      _ccWindowStart = currentTime;
      _ccCount = 0;
    }
    if (ccNumber != MIDI_CC_ALL_SOUND_OFF && ccNumber != MIDI_CC_RESET_ALL_CONTROLLERS && ccNumber != MIDI_CC_ALL_NOTES_OFF) {
      _ccCount++;
      if (_ccCount > CC_RATE_LIMIT_PER_SECOND) {
        return;
      }
    }
  }

  _lastCCTime = currentTime;

  switch (ccNumber) {
    case MIDI_CC_MODULATION:  // Vibrato
      _ccModulation = ccValue;
      _airflowCtrl.setCCValues(_ccVolume, _ccExpression, _ccModulation);
      if (DEBUG) {
        Serial.print("DEBUG: CC 1 (Modulation) = ");
        Serial.println(ccValue);
      }
      break;

    case MIDI_CC_BREATH:  // Breath Controller
      _ccBreath = ccValue;
      _airflowCtrl.updateCC2Breath(ccValue);
      break;

    case MIDI_CC_VOLUME:  // Volume
      _ccVolume = ccValue;
      _airflowCtrl.setCCValues(_ccVolume, _ccExpression, _ccModulation);
      if (DEBUG) {
        Serial.print("DEBUG: CC 7 (Volume) = ");
        Serial.println(ccValue);
      }
      break;

    case MIDI_CC_EXPRESSION:  // Expression
      _ccExpression = ccValue;
      _airflowCtrl.setCCValues(_ccVolume, _ccExpression, _ccModulation);
      if (DEBUG) {
        Serial.print("DEBUG: CC 11 (Expression) = ");
        Serial.println(ccValue);
      }
      break;

    case MIDI_CC_ATTACK_TIME:  // Attack Time (mode attaque souffle)
      _airflowCtrl.setCC73Attack(ccValue);
      if (DEBUG) {
        Serial.print("DEBUG: CC 73 (Attack Time) = ");
        Serial.println(ccValue);
      }
      break;

    case MIDI_CC_BRIGHTNESS:  // Brightness -> angle servo (trav)
      _ccBrightness = ccValue;
      _airflowCtrl.setCC74Brightness(ccValue);
      if (DEBUG) {
        Serial.print("DEBUG: CC 74 (Brightness/Angle) = ");
        Serial.println(ccValue);
      }
      break;

    case MIDI_CC_ALL_SOUND_OFF:
      allSoundOff();
      break;

    case MIDI_CC_RESET_ALL_CONTROLLERS:
      resetAllControllers();
      break;

    case MIDI_CC_ALL_NOTES_OFF:
      allSoundOff();
      break;

    default:
      break;
  }
}

void InstrumentManager::allSoundOff() {
  while (!_eventQueue.isEmpty()) {
    _eventQueue.dequeue();
  }
  _sequencer.stop();
  _airflowCtrl.closeSolenoid();
  _airflowCtrl.setAirflowToRest();  // inclut setAngleToRest()
  _fingerCtrl.closeAllFingers();
  if (cfg.airMode == AIR_MODE_FAN_SERVO) {
    _fanCtrl.stop();
  }
  if (cfg.airMode >= AIR_MODE_PUMP_VALVE) {
    _pressureCtrl.stop();
  }

  if (DEBUG) {
    Serial.println("DEBUG: InstrumentManager - All Sound Off");
  }
}

void InstrumentManager::resetAllControllers() {
  _ccVolume = cfg.ccVolumeDefault;
  _ccExpression = cfg.ccExpressionDefault;
  _ccModulation = cfg.ccModulationDefault;
  _ccBreath = cfg.ccBreathDefault;
  _ccBrightness = cfg.ccBrightnessDefault;
  _airflowCtrl.setCCValues(_ccVolume, _ccExpression, _ccModulation);
  _airflowCtrl.setCC74Brightness(_ccBrightness);

  if (DEBUG) {
    Serial.println("DEBUG: InstrumentManager - Reset All Controllers");
  }
}

void InstrumentManager::setPWM(uint8_t channel, uint16_t on, uint16_t off) {
  if (channel < 16) {
    _pwm0.setPWM(channel, on, off);
  } else if (_secondBoardEnabled) {
    _pwm1.setPWM(channel - 16, on, off);
  }
}

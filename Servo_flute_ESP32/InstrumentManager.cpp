#include "InstrumentManager.h"
#include "ConfigStorage.h"
// Table centrale de la topologie materielle (LOT B) : configChangeRequiresRestart()
// n'est plus qu'un relais vers elle.
#include "ConfigTopology.h"
#include <Wire.h>

InstrumentManager::InstrumentManager()
  : _pwm0(PCA_ADDR_BOARD0),
    _pwm1(PCA_ADDR_BOARD1),
    _secondBoardEnabled(false),
    _pca0Detected(false),
    _pca1Detected(false),
    _hardwareProbeDone(false),
    _hardwareInitStatus(HW_CONFIG_INVALID),
    _eventQueue(EVENT_QUEUE_SIZE),
    _commands(COMMAND_QUEUE_SIZE),
    _fingerCtrl([this](uint8_t ch, uint16_t on, uint16_t off) { setPWM(ch, on, off); }),
    _airflowCtrl([this](uint8_t ch, uint16_t on, uint16_t off) { setPWM(ch, on, off); }),
    _calAirSupply(_pressureCtrl, _fanCtrl),
    _sequencer(_eventQueue, _fingerCtrl, _airflowCtrl),
    _lastActivityTime(0),
    _servosPowered(false),
    _actuatorSessionActive(false),
    _initializingHardware(false),
    _ccVolume(cfg.ccVolumeDefault),
    _ccExpression(cfg.ccExpressionDefault),
    _ccModulation(cfg.ccModulationDefault),
    _ccBreath(cfg.ccBreathDefault),
    _ccBrightness(cfg.ccBrightnessDefault),
    _ccCount(0),
    _ccWindowStart(0),
    _cc2Count(0),
    _cc2WindowStart(0),
    _cc2Pending(false),
    _cc2PendingValue(0),
    _powerOnRequested(false),
    _resetControllersRequested(false),
    _noteOffDeferrals(0),
    _panicCount(0),
    _prevSequencerState(STATE_IDLE),
    _prevNoteSounding(false),
    _timingObserver(nullptr),
    _audioObserver(nullptr) {
}

void InstrumentManager::setTimingObserver(AcousticTiming* obs) {
  // Simple transmission d'un pointeur : aucun appel n'est fait sur l'observateur
  // ici, aucune consigne d'actionneur n'est (re)calculee, et poser ou retirer un
  // observateur en pleine note ne touche ni la valve, ni le souffle, ni la
  // pompe. Les seuls a le connaitre sont ceux qui ont un instant d'ordre a
  // signaler : le sequenceur (note commandee / relachee) et le controleur de
  // souffle (consigne d'air / valve ouverte).
  _timingObserver = obs;
  _sequencer.setTimingObserver(obs);
  _airflowCtrl.setTimingObserver(obs);
}

void InstrumentManager::setAudioObserver(IAudioSource* obs) {
  // MEME discipline que setTimingObserver() juste au-dessus : simple
  // transmission d'un pointeur. Aucun appel n'est fait sur l'observateur ici,
  // aucune consigne d'actionneur n'est (re)calculee, et poser ou retirer un
  // observateur en pleine note ne touche ni la valve, ni le souffle, ni la
  // pompe.
  //
  // UN destinataire, pas deux : le sequenceur. Ce signal dit "la note a
  // change", et les deux seuls instants ou une note change sont ses deux
  // bornes. Le controleur de souffle ne possede que des instants d'ordre
  // INTERMEDIAIRES (consigne d'air, valve ouverte), qui tombent AU MILIEU
  // d'une note : lui donner cet observateur ferait effacer l'historique
  // acoustique de la note en cours a chaque ouverture de valve, c'est-a-dire
  // juste avant la frame qu'on cherche precisement a mesurer.
  _audioObserver = obs;
  _sequencer.setAudioObserver(obs);
}

void InstrumentManager::begin() {
  beginSafe();
}

bool InstrumentManager::detectPca(uint8_t address) const {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

bool InstrumentManager::requiresSecondPca() const {
  for (int i = 0; i < cfg.numFingers; i++) {
    if (cfg.fingers[i].pcaChannel >= 16) return true;
  }
  return cfg.airflowPcaChannel >= 16 ||
         (modeUsesPhysicalValve(cfg.airMode) && cfg.valveType == 1 && cfg.valveServoPcaChannel >= 16) ||
         (cfg.angleServoEnabled && cfg.angleServoPcaChannel >= 16);
}

bool InstrumentManager::beginSafe() {
  if (DEBUG) Serial.println("DEBUG: InstrumentManager - Initialisation sure");

  pinMode(PIN_SERVOS_OFF, OUTPUT);
  digitalWrite(PIN_SERVOS_OFF, HIGH);
  _servosPowered = false;
  _initializingHardware = true;   // program safe PWM values without enabling OE
  _secondBoardEnabled = requiresSecondPca();
  _hardwareInitStatus = HW_CONFIG_INVALID;
  _commands.clear();
  _eventQueue.clear();

  // Le resultat du sondage I2C est memorise : les diagnostics exposent l'etat
  // REEL des cartes plutot qu'un "probe requires hardware" generique.
  // Mettre TOUTES les sorties d'actionneurs configurables a leur niveau inactif
  // AVANT de sonder l'I2C. Si le sondage echoue, beginSafe() sort en erreur sans
  // jamais appeler PressureController::begin() / FanController::begin() : les
  // broches de pompe et de ventilateur restaient alors en haute impedance,
  // c'est-a-dire une grille de MOSFET flottante. (Une resistance de pull-down
  // materielle reste la protection de reference - voir HARDWARE_TEST_MATRIX.)
  driveConfiguredActuatorPinsInactive();

  _hardwareProbeDone = true;
  _pca0Detected = detectPca(PCA_ADDR_BOARD0);
  _pca1Detected = detectPca(PCA_ADDR_BOARD1);

  if (!_pca0Detected) {
    _hardwareInitStatus = HW_PCA0_MISSING;
    _initializingHardware = false;   // failed: setPWM must now refuse all writes
    return false;
  }
  if (_secondBoardEnabled && !_pca1Detected) {
    _hardwareInitStatus = HW_PCA1_MISSING;
    _initializingHardware = false;
    return false;
  }

  _pwm0.begin();
  _pwm0.setPWMFreq(SERVO_FREQUENCY);
  delay(PWM_INIT_DELAY_MS);
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
    if (cfg.airMode == AIR_MODE_PUMP_RESERVOIR && cfg.reservoirAutoStart) {
      if (sensorOk) _pressureCtrl.setTargetPercent(cfg.reservoirTargetPercent);
      else _pressureCtrl.stop();
    }
  }
  if (cfg.airMode == AIR_MODE_FAN_SERVO) {
    _fanCtrl.begin();
    _fanCtrl.stop();
  }

  _airflowCtrl.setCCValues(_ccVolume, _ccExpression, _ccModulation);
  _sequencer.begin();
  _lastActivityTime = millis();
  _hardwareInitStatus = HW_INIT_OK;
  // All channels now hold safe values: end the init window and enable OE exactly
  // once, so the servos move cleanly to their programmed safe positions.
  _initializingHardware = false;
  powerOnServos();
  return true;
}

void InstrumentManager::driveConfiguredActuatorPinsInactive() {
  // La configuration est deja validee a ce stade (le boot refuse de construire
  // l'InstrumentManager sinon), donc ces numeros de broches sont surs.
  if (configurationUsesSolenoidValve(cfg)) {
    pinMode(cfg.solenoidPin, OUTPUT);
    digitalWrite(cfg.solenoidPin, SOLENOID_ACTIVE_HIGH ? LOW : HIGH);
  }
  if (configurationUsesFan(cfg)) {
    pinMode(cfg.fanPin, OUTPUT);
    digitalWrite(cfg.fanPin, LOW);
  }
  if (configurationUsesPumps(cfg)) {
    for (uint8_t i = 0; i < cfg.numPumps && i < MAX_PUMPS; i++) {
      pinMode(cfg.pumpPins[i], OUTPUT);
      digitalWrite(cfg.pumpPins[i], LOW);
    }
  }
}

void InstrumentManager::initializeSafeOutputs() {
  _fingerCtrl.closeAllFingers();
  _airflowCtrl.closeValve();
  _airflowCtrl.setAirflowToRest();
  if (cfg.angleServoEnabled) _airflowCtrl.setAngleToRest();
}

void InstrumentManager::update() {
  // Les commandes venues des autres taches (AsyncTCP/WebSocket, NimBLE) sont
  // appliquees ICI, sur la tache proprietaire des actionneurs. C'est le seul
  // endroit ou une commande web touche le bus I2C ou un GPIO d'actionneur.
  // Le drainage a lieu meme quand le hardware n'est pas pret : applyCommand()
  // refuse alors toute commande physique, mais la file ne se remplit pas
  // indefiniment et un panic reste consomme.
  processCommands();

  if (_hardwareInitStatus != HW_INIT_OK) return;   // failed init: keep everything inert

  serviceCc2Coalescing(millis());
  // While an actuator session (auto-calibration / range finder) owns the hardware,
  // the MIDI sequencer must not drive the finger/airflow servos or the valve - the
  // calibrator moves them directly. Skipping the sequencer here (plus rejecting
  // noteOn/noteOff/CC below) keeps external MIDI from corrupting the measurement.
  if (!_actuatorSessionActive) _sequencer.update();
  _airflowCtrl.update();
  // Traduction "etat du sequenceur + souffle reel -> consigne pompe/ventilateur".
  // Placee APRES _airflowCtrl.update() pour lire le souffle de CE tour, et
  // DESACTIVEE pendant une session d'actionneurs : c'est alors
  // CalibrationAirSupply qui possede la source d'air, et la laisser tourner ici
  // ecraserait sa demande a chaque boucle.
  if (!_actuatorSessionActive) updateAirSourceFromSequencer();
  if (cfg.airMode >= AIR_MODE_PUMP_VALVE) {
    _pressureCtrl.update();
  }
  if (cfg.airMode == AIR_MODE_FAN_SERVO) {
    _fanCtrl.update();
  }
  managePower();
}

void InstrumentManager::noteOn(byte midiNote, byte velocity) {
  if (_hardwareInitStatus != HW_INIT_OK) return;   // no usable hardware
  // Calibration owns the actuators: ignore external MIDI (BLE / rtpMIDI / DIN /
  // file player) so it cannot move fingers, the airflow servo, the valve, the
  // pumps or the fan while a measurement is running.
  if (_actuatorSessionActive) return;
  if (!isNotePlayable(midiNote)) {
    if (DEBUG) {
      Serial.print("DEBUG: InstrumentManager - Note hors plage: ");
      Serial.println(midiNote);
    }
    return;
  }

  // NB: the pump / fan demand is NOT set here. Driving it from the raw incoming
  // MIDI event would let a full event queue start the pump for a note that never
  // plays. It is instead applied when the sequencer actually starts the note
  // (updateAirSourceFromSequencer), so demand always tracks a real note.
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

  registerActuatorActivity();
}

void InstrumentManager::noteOff(byte midiNote) {
  if (_hardwareInitStatus != HW_INIT_OK) return;   // no usable hardware
  if (_actuatorSessionActive) return;   // calibration owns the actuators
  // The pump is NOT returned to idle here: a stale NOTE_OFF for an already-replaced
  // note would otherwise cut the air under the new note. Idle is applied only when
  // the sequencer truly returns to STATE_IDLE (updateAirSourceFromSequencer).
  // Forced enqueue: a dropped NOTE_OFF on a full queue would strand the note
  // (valve/air kept on) until the next note-on or a panic. Evict the oldest
  // event instead so the release always lands.
  bool success = _eventQueue.enqueueLiveEventForced(EVENT_NOTE_OFF, midiNote, 0);

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

uint8_t InstrumentManager::computePumpDemand(byte velocity) const {
  uint8_t demand = cfg.pumpFollowAirflow
                       ? (uint8_t)((uint16_t)cfg.pumpDirectMaxPercent * velocity / 127)
                       : cfg.pumpDirectMaxPercent;
  if (demand < cfg.pumpDirectIdlePercent) demand = cfg.pumpDirectIdlePercent;
  return demand;
}

uint8_t InstrumentManager::computeFanDemand(byte velocity) const {
  uint8_t demand = cfg.fanFollowAirflow
                       ? (uint8_t)((uint16_t)cfg.fanMaxNotePercent * velocity / 127)
                       : cfg.fanMaxNotePercent;
  if (demand < cfg.fanDefaultPercent) demand = cfg.fanDefaultPercent;
  return demand;
}

void InstrumentManager::updateAirSourceFromSequencer() {
  // Only the velocity-driven air modes are handled here. AIR_MODE_PUMP_RESERVOIR
  // (5) regulates to a fixed reservoir target and must not be pulled per note.
  bool directPump = (cfg.airMode == AIR_MODE_PUMP_VALVE);
  bool fan = (cfg.airMode == AIR_MODE_FAN_SERVO);
  if (!directPump && !fan) return;

  NoteState curState = _sequencer.getState();
  // Le souffle effectif, pas seulement l'etat du sequenceur : CC2 peut faire
  // taire une note TENUE sans aucune transition d'etat. Sans ce second critere,
  // la pompe restait a 100 % alors que la valve venait d'etre fermee.
  bool sounding = _airflowCtrl.isNoteSounding();
  if (curState == _prevSequencerState && sounding == _prevNoteSounding) return;

  // POSITIONING est volontairement traite comme "demande de jeu" : la source
  // d'air a besoin de ce temps d'avance pour monter avant l'ouverture de la
  // valve. C'est seulement une note PLAYING rendue muette par CC2 qui redescend
  // au ralenti.
  bool wantPlayDemand = (curState == STATE_POSITIONING) ||
                        (curState == STATE_PLAYING && sounding);

  if (wantPlayDemand) {
    byte vel = _sequencer.getCurrentVelocity();
    if (directPump) {
      _pressureCtrl.setTargetPercent(computePumpDemand(vel));
    } else {
      _fanCtrl.onNoteOn();
      _fanCtrl.setSpeed(computeFanDemand(vel));
    }
  } else {
    // Plus de note qui sonne (fin de note, ou silence demande par CC2).
    if (directPump) {
      _pressureCtrl.setTargetPercent(cfg.pumpDirectIdlePercent);
    } else {
      _fanCtrl.onNoteOff();
    }
  }

  _prevSequencerState = curState;
  _prevNoteSounding = sounding;
}

void InstrumentManager::managePower() {
  // An external actuator session (auto-calibration / range finder) drives the
  // servos and reads audio without going through the MIDI sequencer. The idle
  // power-down does not see that activity, so it must be inhibited: otherwise the
  // PCA9685 OE (and the finger/airflow servos) could be cut mid-measurement.
  if (_actuatorSessionActive) {
    ensureServosPowered();
    _lastActivityTime = millis();
    return;
  }
  // Prise ATOMIQUE : lue et effacee sous un seul verrou. Une demande deposee
  // par une autre tache pendant ensureServosPowered() reste posee et sera
  // honoree a la passe suivante, au lieu d'etre effacee sans effet.
  if (takePowerOnRequest()) {
    ensureServosPowered();
  }
  if (cfg.timeUnpower == 0) {
    ensureServosPowered();
    return;
  }
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

void InstrumentManager::ensureServosPowered() {
  if (!_servosPowered) {
    powerOnServos();
  }
}

void InstrumentManager::registerActuatorActivity() {
  // Peut etre appelee depuis une tache productrice (enfilement d'une note). On
  // ne touche donc PAS au GPIO d'OE ici : on note l'activite et on demande
  // l'alimentation ; managePower() (tache loop()) execute l'ecriture.
  //
  // CHEMIN BRULANT : setPWM() passe par ici a chaque ecriture de servo. La
  // section critique se limite donc au depot du drapeau - millis() est lu AVANT,
  // hors verrou, pour ne pas garder les interruptions masquees pendant la
  // lecture du compteur.
  const unsigned long now = millis();
  portENTER_CRITICAL(&_requestMux);
  _powerOnRequested = true;
  portEXIT_CRITICAL(&_requestMux);
  // Horodatage volontairement HORS verrou : ecriture unique d'un mot aligne,
  // sans lecture-modification-ecriture, et la semantique voulue est "le dernier
  // qui ecrit gagne". managePower() le relit une seule fois par passe.
  _lastActivityTime = now;
}

bool InstrumentManager::takePowerOnRequest() {
  // Lecture ET effacement en UNE section critique - meme motif que
  // CommandQueue::takePanicRequest(). C'est ce qui rend impossible la perte
  // d'une demande deposee entre les deux.
  portENTER_CRITICAL(&_requestMux);
  bool requested = _powerOnRequested;
  _powerOnRequested = false;
  portEXIT_CRITICAL(&_requestMux);
  return requested;
}

bool InstrumentManager::takeResetControllersRequest() {
  portENTER_CRITICAL(&_requestMux);
  bool requested = _resetControllersRequested;
  _resetControllersRequested = false;
  portEXIT_CRITICAL(&_requestMux);
  return requested;
}

bool InstrumentManager::powerOnRequestPending() const {
  portENTER_CRITICAL(&_requestMux);
  bool requested = _powerOnRequested;
  portEXIT_CRITICAL(&_requestMux);
  return requested;
}

bool InstrumentManager::resetControllersRequestPending() const {
  portENTER_CRITICAL(&_requestMux);
  bool requested = _resetControllersRequested;
  portEXIT_CRITICAL(&_requestMux);
  return requested;
}

void InstrumentManager::setActuatorSessionActive(bool active) {
  // IDEMPOTENT, et c'est essentiel. L'appelant (WebConfigurator) pouvait
  // reclamer la session a CHAQUE tour de boucle pendant une calibration. Comme
  // la prise de possession appelle _sequencer.stop(), qui ferme la valve et
  // ramene le servo de souffle au repos, le calibrateur se faisait ecraser en
  // permanence : il positionne son angle une seule fois (ST_SET) puis mesure
  // pendant ST_SETTLE/ST_COLLECT sans jamais reappliquer la consigne. Il
  // mesurait donc valve fermee et servo au repos. Toute re-demande identique est
  // desormais un no-op (on se contente de maintenir l'alimentation servo).
  if (_actuatorSessionActive == active) {
    if (active) ensureServosPowered();
    return;
  }

  _actuatorSessionActive = active;

  if (active) {
    // Release any note the sequencer was playing and drop queued MIDI so nothing
    // fires while the calibrator owns the actuators, then hold servo power.
    _sequencer.stop();
    _eventQueue.clear();
    // LES COMMANDES D'ACTIONNEURS AUSSI. _eventQueue.clear() ne couvre que les
    // evenements MIDI ; une commande web deposee par la tache AsyncTCP et pas
    // encore consommee par update() survivait a la prise de possession et
    // s'appliquait PENDANT la mesure - un angle de doigt, une consigne de
    // pompe, l'ouverture de la valve - sur un instrument que le calibrateur
    // croit posseder seul.
    //
    // clear() conserve les ordres d'ARRET et le panic (voir CommandQueue.cpp) :
    // ce qui disparait ici, ce sont les commandes ordinaires, jamais une mise
    // en securite.
    _commands.clear();
    // Aligner le suivi de transition : sans cela, le retour force a STATE_IDLE
    // serait lu au tour suivant comme une fin de note normale et remettrait la
    // pompe / le ventilateur a leur consigne de repos, par-dessus la demande que
    // le calibrateur vient d'etablir via CalibrationAirSupply.
    _prevSequencerState = STATE_IDLE;
    _prevNoteSounding = false;
    ensureServosPowered();
  }
}

void InstrumentManager::powerOnServos() {
  // Never energise the servos before a successful safe init (nor during the init
  // window itself); OE stays HIGH until beginSafe() has programmed every channel.
  if (_initializingHardware || _hardwareInitStatus != HW_INIT_OK) return;
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

// CC 120-127 sont les "Channel Mode Messages" de la norme MIDI. Ils portent les
// commandes de securite (All Sound Off, Reset All Controllers, All Notes Off,
// Omni/Mono/Poly qui impliquent All Notes Off). Ils ne doivent JAMAIS etre
// rejetes par le limiteur de debit : les jeter laisserait une note soufflee avec
// la valve ouverte alors que le controleur vient justement de demander l'arret.
static inline bool isChannelModeControlChange(byte cc) {
  return cc >= 120 && cc <= 127;
}

void InstrumentManager::applyBreathValue(byte ccValue) {
  _ccBreath = ccValue;
  _airflowCtrl.updateCC2Breath(ccValue);
  _airflowCtrl.recomputeActiveNote();   // breath silences/resumes the held note
}

void InstrumentManager::serviceCc2Coalescing(unsigned long now) {
  if (!_cc2Pending) return;
  if (_actuatorSessionActive) { _cc2Pending = false; return; }
  if ((int32_t)(now - _cc2WindowStart) >= (int32_t)CC_RATE_WINDOW_MS) {
    _cc2WindowStart = now;
    _cc2Count = 0;
  }
  if (_cc2Count >= CC2_RATE_LIMIT_PER_SECOND) return;   // fenetre encore saturee
  _cc2Count++;
  byte value = _cc2PendingValue;
  _cc2Pending = false;
  applyBreathValue(value);
}

void InstrumentManager::handleControlChange(byte ccNumber, byte ccValue) {
  if (ccValue > MIDI_CC_MAX) {
    if (DEBUG) {
      Serial.print("ERREUR: CC invalide - valeur: ");
      Serial.println(ccValue);
    }
    return;
  }

  // --- 1. Messages de mode canal (120-127) : jamais limites, jamais differes ---
  // Traites AVANT toute comptabilite de debit et meme pendant une session
  // d'actionneurs : ce sont les commandes d'arret.
  if (isChannelModeControlChange(ccNumber)) {
    switch (ccNumber) {
      case MIDI_CC_ALL_SOUND_OFF:            // 120
      case MIDI_CC_ALL_NOTES_OFF:            // 123
      case MIDI_CC_OMNI_OFF:                 // 124
      case 125:                              // Omni On
      case MIDI_CC_MONO_ON:                  // 126
      case 127:                              // Poly On
        // Panique a part entiere : ces CC sont les commandes d'arret de la
        // norme MIDI et peuvent arriver par N'IMPORTE QUEL transport (BLE,
        // rtpMIDI, DIN, fichier), dont aucun ne traverse le moindre code web.
        executePanic();
        break;
      case MIDI_CC_RESET_ALL_CONTROLLERS:    // 121
        resetAllControllers();
        break;
      case 122:                              // Local Control: sans objet ici
      default:
        break;
    }
    return;
  }

  if (_hardwareInitStatus != HW_INIT_OK) return;   // no usable hardware
  if (_actuatorSessionActive) return;   // calibration owns the actuators

  unsigned long currentTime = millis();

  // --- 2. CC2 (Breath Controller) : coalescence, jamais de rejet sec ---
  if (ccNumber == MIDI_CC_BREATH) {
    if (cfg.cc2Enabled) {
      // Une demande de silence (valeur sous le seuil) est TOUJOURS appliquee
      // immediatement : c'est elle qui coupe le souffle.
      bool silenceRequest = (ccValue <= cfg.cc2SilenceThreshold);
      if (currentTime - _cc2WindowStart >= CC_RATE_WINDOW_MS) {
        _cc2WindowStart = currentTime;
        _cc2Count = 0;
      }
      if (!silenceRequest && _cc2Count >= CC2_RATE_LIMIT_PER_SECOND) {
        // Fenetre saturee : on conserve la DERNIERE valeur recue au lieu de la
        // jeter. serviceCc2Coalescing() l'appliquera des la fenetre suivante, donc
        // une rafale de CC2 ne peut plus laisser la note souffler indefiniment.
        _cc2Pending = true;
        _cc2PendingValue = ccValue;
        return;
      }
      _cc2Count++;
      _cc2Pending = false;
    }
    applyBreathValue(ccValue);
    return;
  }

  // --- 3. Autres CC : limiteur de debit classique ---
  if (currentTime - _ccWindowStart >= CC_RATE_WINDOW_MS) {
    _ccWindowStart = currentTime;
    _ccCount = 0;
  }
  _ccCount++;
  if (_ccCount > CC_RATE_LIMIT_PER_SECOND) {
    return;
  }


  switch (ccNumber) {
    case MIDI_CC_MODULATION:  // Vibrato
      _ccModulation = ccValue;
      _airflowCtrl.setCCValues(_ccVolume, _ccExpression, _ccModulation);
      _airflowCtrl.recomputeActiveNote();   // apply to the held note immediately
      if (DEBUG) {
        Serial.print("DEBUG: CC 1 (Modulation) = ");
        Serial.println(ccValue);
      }
      break;

    case MIDI_CC_VOLUME:  // Volume
      _ccVolume = ccValue;
      _airflowCtrl.setCCValues(_ccVolume, _ccExpression, _ccModulation);
      _airflowCtrl.recomputeActiveNote();   // apply to the held note immediately
      if (DEBUG) {
        Serial.print("DEBUG: CC 7 (Volume) = ");
        Serial.println(ccValue);
      }
      break;

    case MIDI_CC_EXPRESSION:  // Expression
      _ccExpression = ccValue;
      _airflowCtrl.setCCValues(_ccVolume, _ccExpression, _ccModulation);
      _airflowCtrl.recomputeActiveNote();   // apply to the held note immediately
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

    default:
      break;
  }
}

/*******************************************************************************
 * File de commandes inter-taches
 ******************************************************************************/

bool InstrumentManager::commandDrivesActuators(uint8_t type) {
  switch (type) {
    // Commandes sans effet physique (etat interne / arret).
    case ACMD_NONE:
    case ACMD_ALL_SOUND_OFF:
    case ACMD_RESET_CONTROLLERS:
    case ACMD_PUMP_STOP:
    case ACMD_PUMP_STOP_SINGLE:
    case ACMD_FAN_STOP:
    case ACMD_SET_ACTUATOR_SESSION:
      return false;
    default:
      return true;
  }
}

bool InstrumentManager::commandMayEnergizeActuator(const ActuatorCommand& cmd) {
  switch (cmd.type) {
    // --- Ne peuvent que RETIRER de l'energie, ou ne touchent aucun actionneur.
    case ACMD_NONE:
    case ACMD_ALL_SOUND_OFF:
    case ACMD_NOTE_OFF:
    case ACMD_PUMP_STOP:
    case ACMD_PUMP_STOP_SINGLE:
    case ACMD_FAN_STOP:
    case ACMD_SET_ACTUATOR_SESSION:
      return false;

    // --- Variantes : seul le sens "vers zero" retire de l'energie. Ce sont
    // exactement les cinq routages imperdables de postCommand() ; les laisser
    // passer est ce qui garde un arret possible pendant une calibration.
    case ACMD_PUMP_ENABLE:   return cmd.a != 0;
    case ACMD_TEST_SOLENOID: return cmd.a != 0;
    case ACMD_PUMP_TARGET:   return cmd.b != 0;
    case ACMD_FAN_TARGET:    return cmd.b != 0;

    // --- ACMD_PUMP_SINGLE_TEST est energisant QUEL QUE SOIT b, et ce n'est pas
    // une approximation : il pose _testPumpIndex, ce qui court-circuite la
    // regulation dans PressureController::update() et confisque la pompe. Meme
    // a 0 %, c'est une prise de possession - et l'arret correspondant existe
    // deja par ailleurs (ACMD_PUMP_STOP_SINGLE), qui lui passe.
    //
    // ACMD_RESET_CONTROLLERS tombe aussi ici : remettre les CC a leurs valeurs
    // par defaut peut AUGMENTER le souffle (ccBreathDefault vaut 127 par
    // defaut). Ce n'est pas un ordre de securite - CC120/123 le sont, et eux
    // sont routes vers le panic bien avant d'arriver ici.
    default:
      return true;
  }
}

bool InstrumentManager::postCommand(const ActuatorCommand& cmd) {
  // Les commandes de securite ne transitent JAMAIS par l'anneau : elles ne
  // doivent pas pouvoir etre perdues par saturation.
  if (cmd.type == ACMD_ALL_SOUND_OFF) {
    requestPanic();
    return true;
  }
  // ORDRES D'ARRET : ils RETIRENT de l'energie, donc ils ne transitent pas non
  // plus par l'anneau (voir CommandQueue::requestStop). `push()` rendait false
  // sur un anneau plein et l'appelant web ignore cette valeur : l'ordre d'arret
  // etait jete ET, cote WebConfigurator, endTestSession(false) supprimait dans
  // la foulee le timeout de securite de la session d'essai. La pompe restait
  // alors alimentee a sa consigne, sans limite de duree. Ces cinq routages
  // rendent donc TOUJOURS true.
  //
  // L'ASYMETRIE EST VOULUE : seule la variante qui retire de l'energie quitte
  // l'anneau. `pump_enable = true` et `test_sol = 1` en AJOUTENT et restent
  // ordinaires - perdre une mise en route est sur, perdre un arret ne l'est pas.
  switch (cmd.type) {
    case ACMD_PUMP_STOP:
      _commands.requestStop(CommandQueue::STOPREQ_PUMPS);
      return true;
    case ACMD_FAN_STOP:
      _commands.requestStop(CommandQueue::STOPREQ_FAN);
      return true;
    case ACMD_PUMP_STOP_SINGLE:
      _commands.requestPumpStopSingle(cmd.a);
      return true;
    case ACMD_PUMP_ENABLE:
      if (cmd.a == 0) {
        _commands.requestStop(CommandQueue::STOPREQ_PUMPS_OFF);
        return true;
      }
      break;
    // CONSIGNE A ZERO = retrait d'energie. Les curseurs de l'interface web ne
    // passent pas par pump_stop / fan_stop pour ramener un actionneur a zero :
    // ils envoient pump_target / fan_target avec v = 0. Cette intention-la
    // restait dans l'anneau ordinaire et se perdait quand il etait plein -
    // l'actionneur gardait alors sa consigne precedente. Le bit dedie applique
    // EXACTEMENT la meme commande (voir processCommands), pas un arret dur.
    case ACMD_PUMP_TARGET:
      if (cmd.b == 0) {
        _commands.requestStop(CommandQueue::STOPREQ_PUMP_TARGET_ZERO);
        return true;
      }
      break;
    case ACMD_FAN_TARGET:
      if (cmd.b == 0) {
        _commands.requestStop(CommandQueue::STOPREQ_FAN_TARGET_ZERO);
        return true;
      }
      break;
    case ACMD_TEST_SOLENOID:
      if (cmd.a == 0) {
        _commands.requestStop(CommandQueue::STOPREQ_SOLENOID);
        return true;
      }
      break;
    default:
      break;
  }
  if (cmd.type == ACMD_NOTE_OFF) {
    // Un relachement ne doit jamais etre perdu par saturation de l'anneau :
    // il partirait sinon avec la note, la valve et le souffle encore ouverts.
    //
    // L'ANNEAU D'ABORD, le bitmap en REPLI. Router systematiquement les Note Off
    // vers le bitmap les sortait de l'ordre FIFO : "Note Off 60 puis Note On 60"
    // emis entre deux tours de loop() s'appliquait "Note On puis Note Off" et la
    // note finissait muette. Dans l'anneau, l'ordre d'emission est respecte par
    // construction ; et quand l'anneau REFUSE, le bitmap le rattrape, ce qui est
    // exactement l'hypothese sur laquelle processCommands() fonde son ordre
    // d'application (voir son commentaire). pushOrFallback() ne compte pas ce
    // refus comme une perte : la commande n'est pas perdue, elle change de canal.
    if (!_commands.pushOrFallback(cmd)) _commands.requestNoteOff(cmd.a);
    return true;
  }
  if (cmd.type == ACMD_CONTROL_CHANGE && isChannelModeControlChange(cmd.a)) {
    if (cmd.a == MIDI_CC_RESET_ALL_CONTROLLERS) {
      // Depot sous le verrou des demandes : processCommands() PREND le drapeau
      // (lecture + effacement atomiques), donc une demande posee ici pendant
      // qu'il traite la precedente n'est jamais effacee sans avoir ete traitee.
      portENTER_CRITICAL(&_requestMux);
      _resetControllersRequested = true;
      portEXIT_CRITICAL(&_requestMux);
      return true;
    }
    if (cmd.a == 122) return true;   // Local Control: sans objet
    requestPanic();
    return true;
  }
  return _commands.push(cmd);
}

bool InstrumentManager::postCommand(uint8_t type, uint8_t a, uint8_t b, uint16_t c) {
  return postCommand(ActuatorCommand(type, a, b, c));
}

void InstrumentManager::requestPanic() {
  _commands.requestPanic();
}

void InstrumentManager::processCommands() {
  // Le panic prime sur tout : il a deja vide la file cote CommandQueue.
  // CONSOMMATION du panic : c'est ici qu'il est reellement execute, donc ici
  // qu'il se compte. requestPanic() peut etre appelee dix fois depuis une tache
  // BLE avant que loop() ne reprenne la main ; CommandQueue les coalesce en un
  // seul drapeau et une seule panique est comptee - une par panique, pas une
  // par demande.
  if (_commands.takePanicRequest()) {
    executePanic();
  }
  // Meme motif que le panic ci-dessus : le drapeau est PRIS (lu et efface sous
  // un seul verrou), jamais teste puis efface en deux temps. Une demande deposee
  // par la tache AsyncTCP pendant resetAllControllers() survit donc et sera
  // traitee a la passe suivante.
  if (takeResetControllersRequest()) {
    resetAllControllers();
  }

  // ORDRES D'ARRET, consommes EN TETE DE PASSE, exactement comme le panic et
  // pour la meme raison : un ordre qui retire de l'energie ne doit jamais
  // attendre derriere INSTRUMENT_MAX_COMMANDS_PER_UPDATE. Place dans l'anneau,
  // il aurait pu patienter jusqu'a COMMAND_QUEUE_SIZE - 1 commandes, soit
  // plusieurs tours de loop(), pendant lesquels la pompe reste alimentee.
  //
  // La PRISE est unique (lecture + effacement sous un seul verrou) : une demande
  // deposee par la tache AsyncTCP apres la prise survit pour la passe suivante.
  // N demandes identiques se sont deja coalescees en un seul bit cote file.
  //
  // Ils passent par applyCommand() et non par les controleurs en direct : c'est
  // le seul point qui porte la protection "hardware_not_ready", et un arret sur
  // un materiel jamais initialise ecrirait sur des GPIO non configures -
  // allSoundOff() refuse deja pour la meme raison.
  //
  // Si un panic a eu lieu ci-dessus, ces arrets sont appliques quand meme : le
  // panic a deja tout eteint, donc ils ne font rien, et les appliquer en trop ne
  // peut que RETIRER de l'energie. Cet ordre evite d'avoir a prouver que le
  // panic couvre exactement chaque bit.
  const uint8_t stopBits = _commands.takeStopRequests();
  // Les consignes a ZERO sont appliquees AVANT les arrets durs : si les deux
  // sont demandes dans la meme passe, c'est l'arret dur - le plus fort des deux
  // - qui a le dernier mot. Elles passent par la commande d'ORIGINE et non par
  // stop() : voir STOPREQ_PUMP_TARGET_ZERO dans CommandQueue.h.
  if (stopBits & CommandQueue::STOPREQ_PUMP_TARGET_ZERO) {
    applyCommand(ActuatorCommand(ACMD_PUMP_TARGET, 0, 0));
  }
  if (stopBits & CommandQueue::STOPREQ_FAN_TARGET_ZERO) {
    applyCommand(ActuatorCommand(ACMD_FAN_TARGET, 0, 0));
  }
  if (stopBits & CommandQueue::STOPREQ_PUMPS) {
    applyCommand(ActuatorCommand(ACMD_PUMP_STOP));
  }
  if (stopBits & CommandQueue::STOPREQ_PUMPS_OFF) {
    applyCommand(ActuatorCommand(ACMD_PUMP_ENABLE, 0));
  }
  if (stopBits & CommandQueue::STOPREQ_FAN) {
    applyCommand(ActuatorCommand(ACMD_FAN_STOP));
  }
  if (stopBits & CommandQueue::STOPREQ_SOLENOID) {
    applyCommand(ActuatorCommand(ACMD_TEST_SOLENOID, 0));
  }
  // Arrets mono-pompe : bitmap par index, tous consommes dans la passe. Ils ne
  // sont PAS bornes - ils sont au plus COMMAND_QUEUE_MAX_PUMP_STOPS et chacun
  // n'ecrit qu'un GPIO de pompe, sans transaction I2C.
  uint8_t stoppedPumpIndex;
  while (_commands.takePendingPumpStop(stoppedPumpIndex)) {
    applyCommand(ActuatorCommand(ACMD_PUMP_STOP_SINGLE, stoppedPumpIndex));
  }

  // TRAVAIL BORNE (voir INSTRUMENT_MAX_COMMANDS_PER_UPDATE). Chaque
  // applyCommand() peut emettre plusieurs transactions I2C : drainer toute la
  // file d'un coup affamait le reste de loop(). Les commandes non traitees
  // restent EN FILE, dans l'ordre, et passent aux tours suivants - l'anneau est
  // FIFO, donc les plus anciennes sortent toujours en premier.
  ActuatorCommand cmd;
  uint8_t appliedThisPass = 0;
  while (appliedThisPass < INSTRUMENT_MAX_COMMANDS_PER_UPDATE && _commands.pop(cmd)) {
    appliedThisPass++;
    applyCommand(cmd);
    // Un panic arrive pendant le drainage annule les commandes restantes.
    if (_commands.panicPending()) {
      _commands.takePanicRequest();
      executePanic();
      _noteOffDeferrals = 0;
      return;
    }
  }

  // Les Note Off en attente sont appliques APRES l'anneau, pas avant. Un Note Off
  // n'atterrit dans le bitmap que parce que l'anneau etait plein, donc tout ce
  // qui le suivait a ete refuse aussi : le traiter en premier pourrait au
  // contraire le faire preceder un Note On deja en file et laisser la note
  // bloquee. Dans le pire cas on perd une note ; jamais on n'en bloque une.
  //
  // CETTE HYPOTHESE EST DESORMAIS GARANTIE, et elle ne l'etait pas. postCommand()
  // envoyait TOUS les Note Off au bitmap, y compris quand l'anneau avait de la
  // place : l'ordre d'emission etait alors perdu dans le cas nominal (un
  // "Note Off puis Note On" sur la meme note s'appliquait a l'envers et la note
  // finissait muette). Le Note Off passe maintenant par l'anneau et ne tombe
  // dans le bitmap que sur REFUS - ce que le raisonnement ci-dessus supposait.
  //
  // La borne ci-dessus ajoute un cas que cet ordre ne prevoyait pas : l'anneau
  // peut rester non vide parce qu'on s'est arrete, pas parce qu'il se remplit.
  // Ceder le pas quelques passes preserve l'ordre voulu ; ceder indefiniment
  // (client qui sature la file en permanence) bloquerait la note, valve et
  // souffle ouverts. On cede donc au plus
  // INSTRUMENT_MAX_NOTE_OFF_DEFERRAL_PASSES fois, puis les relachements passent
  // - meme arbitrage que le paragraphe precedent : perdre une note, jamais en
  // bloquer une.
  if (_commands.count() > 0 && _commands.hasPendingNoteOff() &&
      _noteOffDeferrals < INSTRUMENT_MAX_NOTE_OFF_DEFERRAL_PASSES) {
    _noteOffDeferrals++;
    return;
  }
  _noteOffDeferrals = 0;

  uint8_t releasedThisPass = 0;
  uint8_t pendingNote;
  while (releasedThisPass < INSTRUMENT_MAX_NOTE_OFFS_PER_UPDATE &&
         _commands.takePendingNoteOff(pendingNote)) {
    releasedThisPass++;
    noteOff(pendingNote);
    if (_commands.panicPending()) {
      _commands.takePanicRequest();
      executePanic();
      return;
    }
  }
}

void InstrumentManager::applyCommand(const ActuatorCommand& cmd) {
  // PROTECTION CENTRALE : une commande qui met un actionneur en mouvement est
  // refusee tant que l'initialisation hardware n'a pas reussi (PCA0/PCA1 absents,
  // configuration invalide, systeme de fichiers en panne). C'est le SEUL point
  // d'entree des commandes web/BLE, donc aucun chemin ne peut contourner ce test.
  if (commandDrivesActuators(cmd.type) && _hardwareInitStatus != HW_INIT_OK) {
    if (DEBUG) {
      Serial.print("ERREUR: InstrumentManager - commande actionneur refusee (hardware_not_ready), type ");
      Serial.println(cmd.type);
    }
    return;
  }

  // DEUXIEME PROTECTION CENTRALE : pendant une session d'actionneurs
  // (auto-calibration / range finder), le calibrateur possede le materiel et le
  // pilote par des references DIRECTES sur les controleurs - il n'emprunte
  // jamais cette file. Toute commande qui arrive ici vient donc de l'exterieur
  // (web, MIDI, bouton) et ne doit pas pouvoir ajouter d'energie ni deplacer
  // quoi que ce soit pendant la mesure.
  //
  // POURQUOI CE PREDICAT ET PAS commandDrivesActuators() : une consigne de
  // pompe ou de ventilateur a ZERO "pilote un actionneur", mais c'est le canal
  // imperdable par lequel l'interface COUPE cette pompe. La bloquer rendrait
  // l'arret impossible pendant une calibration - l'inverse du but. Les ordres
  // d'arret, les Note Off et le panic traversent donc tous.
  //
  // C'est ici, et pas dans WebConfigurator, que vit la barriere : ce filtrage-la
  // s'execute sur la tache AsyncTCP et ne peut pas etre la derniere ligne de
  // defense. Cette fonction, elle, est le point unique par lequel toute commande
  // exterieure atteint un actionneur.
  if (_actuatorSessionActive && commandMayEnergizeActuator(cmd)) {
    if (DEBUG) {
      Serial.print("DEBUG: InstrumentManager - commande refusee (session actionneurs), type ");
      Serial.println(cmd.type);
    }
    return;
  }

  switch (cmd.type) {
    case ACMD_NOTE_ON:           noteOn(cmd.a, cmd.b); break;
    case ACMD_NOTE_OFF:          noteOff(cmd.a); break;
    case ACMD_CONTROL_CHANGE:    handleControlChange(cmd.a, cmd.b); break;
    case ACMD_RESET_CONTROLLERS: resetAllControllers(); break;
    case ACMD_ALL_SOUND_OFF:     executePanic(); break;

    case ACMD_TEST_FINGER:
      if (cmd.a < cfg.numFingers) _fingerCtrl.testFingerAngle(cmd.a, cmd.c);
      break;
    case ACMD_TEST_AIRFLOW_ANGLE: _airflowCtrl.testAirflowAngle(cmd.c); break;
    case ACMD_TEST_ANGLE_SERVO:   _airflowCtrl.testAngleServoAngle(cmd.c); break;
    case ACMD_AIR_LIVE_PERCENT:   _airflowCtrl.setAirflowLivePercent(cmd.b); break;
    case ACMD_ANGLE_LIVE_PERCENT: _airflowCtrl.setAngleLivePercent(cmd.b); break;
    case ACMD_TEST_SOLENOID:      _airflowCtrl.testSolenoid(cmd.a != 0); break;

    case ACMD_PUMP_TARGET:        _pressureCtrl.setTargetPercent(cmd.b); break;
    case ACMD_PUMP_SINGLE_TEST:   _pressureCtrl.testSinglePump(cmd.a, cmd.b); break;
    case ACMD_PUMP_STOP_SINGLE:   _pressureCtrl.stopSinglePumpTest(); break;
    case ACMD_PUMP_STOP:          _pressureCtrl.stop(); break;
    case ACMD_PUMP_ENABLE:        _pressureCtrl.setEnabled(cmd.a != 0); break;
    case ACMD_FAN_TARGET:         _fanCtrl.setSpeed(cmd.b); break;
    case ACMD_FAN_STOP:           _fanCtrl.stop(); break;

    case ACMD_OPEN_ALL_FINGERS:
      ensureServosPowered();
      _fingerCtrl.openAllFingers();
      break;

    case ACMD_SET_ACTUATOR_SESSION: setActuatorSessionActive(cmd.a != 0); break;

    default:
      break;
  }
}

bool InstrumentManager::configChangeRequiresRestart(const RuntimeConfig& oldConfig, const RuntimeConfig& newConfig) {
  // SIMPLE RELAIS vers la table centrale de ConfigTopology.
  //
  // La liste qui vivait ici etait ecrite a la main et il en manquait. Les
  // pinMode() du firmware utilisent aussi cfg.endstopPin et cfg.endstopActiveHigh
  // (PressureController.cpp:73, via endstopPinModeFor()) ainsi que cfg.hallPin
  // (PressureController.cpp:95) : aucun des trois n'etait teste. Changer la
  // broche de fin de course a chaud laissait donc l'ANCIENNE configuree en
  // entree et la NOUVELLE jamais initialisee, sans aucune demande de
  // redemarrage - le regulateur lisait alors une broche flottante.
  //
  // Une liste par appelant finit toujours par diverger de ce que begin() fait
  // reellement. Il n'y en a plus qu'une, auditee champ par champ, et cette
  // signature publique est conservee telle quelle pour ses appelants
  // (ConfigCommit.cpp).
  return configTopologyRequiresRestart(oldConfig, newConfig);
}

ConfigApplyResult InstrumentManager::applyRuntimeConfig(const RuntimeConfig& oldConfig, const RuntimeConfig& newConfig) {
  ConfigApplyResult result{true, false, "", ""};

  bool restartNeeded = configChangeRequiresRestart(oldConfig, newConfig);

  result.restartRequired = restartNeeded;
  result.applied = !restartNeeded;
  if (restartNeeded) {
    result.warnings = "GPIO, PCA channels, counts, air mode, sensor type, or MIDI UART changes require restart";
    return result;
  }

  _ccVolume = newConfig.ccVolumeDefault;
  _ccExpression = newConfig.ccExpressionDefault;
  _ccModulation = newConfig.ccModulationDefault;
  _ccBreath = newConfig.ccBreathDefault;
  _ccBrightness = newConfig.ccBrightnessDefault;
  _airflowCtrl.setCCValues(_ccVolume, _ccExpression, _ccModulation);
  _airflowCtrl.setCC74Brightness(_ccBrightness);

  bool fanChanged = oldConfig.fanMinPwm != newConfig.fanMinPwm || oldConfig.fanMaxPwm != newConfig.fanMaxPwm ||
                    oldConfig.fanIdlePercent != newConfig.fanIdlePercent || oldConfig.fanIdleTimeoutMs != newConfig.fanIdleTimeoutMs ||
                    oldConfig.fanDefaultPercent != newConfig.fanDefaultPercent || oldConfig.fanMaxNotePercent != newConfig.fanMaxNotePercent ||
                    oldConfig.fanFollowAirflow != newConfig.fanFollowAirflow;
  bool pressureChanged = oldConfig.pidKp != newConfig.pidKp || oldConfig.pidKi != newConfig.pidKi ||
                         oldConfig.sensorTargetMm != newConfig.sensorTargetMm || oldConfig.pumpCascadeThreshold != newConfig.pumpCascadeThreshold ||
                         oldConfig.pumpStaggerMs != newConfig.pumpStaggerMs;
  if (fanChanged) result.reinitialized += "fan";
  if (pressureChanged) {
    if (result.reinitialized.length() > 0) result.reinitialized += ",";
    result.reinitialized += "pressure";
  }
  registerActuatorActivity();
  return result;
}

uint32_t InstrumentManager::panicCount() const {
  return _panicCount;
}

bool InstrumentManager::safeForBlockingFlashOperation() {
  // UN AUTRE PROPRIETAIRE. Pendant une session d'actionneurs, le calibrateur
  // pilote les controleurs par references DIRECTES et reprendra la main des la
  // fin de l'operation : on ne peut pas garantir l'inertie sur les secondes ou
  // loop() est bloque. Le refus est la seule reponse honnete - l'operateur
  // arrete sa calibration, puis formate.
  if (_actuatorSessionActive) return false;

  allSoundOff();
  // /OE HIGH. C'est ce qu'allSoundOff() ne fait pas, et c'est ce qui compte
  // ici : pas de couple maintenu, pas de courant servo, pendant tout le temps
  // ou plus aucun code de ce projet ne s'execute.
  powerOffServos();
  return true;
}

void InstrumentManager::executePanic() {
  // L'INCREMENT VIENT EN PREMIER, avant l'extinction. allSoundOff() touche le
  // bus I2C et les GPIO d'actionneurs ; si l'une de ces ecritures bloque ou
  // qu'un chien de garde redemarre la carte au milieu, la panique aura quand
  // meme ete comptee pour tout observateur qui lit le compteur d'ici la. Un
  // compteur en avance fait annuler une calibration de trop ; un compteur en
  // retard la laisserait rouvrir la valve juste apres la mise en securite -
  // exactement le defaut qu'on repare. Entre les deux, on choisit le materiel.
  _panicCount++;
  allSoundOff();
}

void InstrumentManager::allSoundOff() {
  // clear() incremente l'epoque de la file : une salve d'evenements en cours de
  // traitement dans NoteSequencer::processDueEvents() est abandonnee, donc aucun
  // evenement anterieur au panic ne peut encore etre joue apres lui.
  _eventQueue.clear();
  // Les commandes actionneurs en attente sont elles aussi abandonnees.
  _commands.clear();
  _cc2Pending = false;
  _sequencer.stop();
  _prevNoteSounding = false;
  // Le panic doit TENIR. updateAirSourceFromSequencer() reagit aux transitions
  // d'etat du sequenceur : sans cette ligne, le retour force a STATE_IDLE serait
  // vu comme "fin normale de note" au tour suivant et remettrait la pompe a sa
  // demande de repos (ou relancerait la rampe de ralenti du ventilateur), juste
  // apres que allSoundOff() les ait arretes. Aligner l'etat precedent supprime
  // la transition : la source d'air reste a l'arret jusqu'a la prochaine VRAIE
  // note, qui appliquera elle-meme sa demande de jeu.
  _prevSequencerState = STATE_IDLE;
  // Hardware jamais initialise (PCA absent / config invalide) : les controleurs
  // n'ont pas configure leurs GPIO, on ne doit rien ecrire dessus. Vider les files
  // et remettre la machine a etats au repos suffit - rien n'a pu etre active.
  if (_hardwareInitStatus != HW_INIT_OK) return;
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

void InstrumentManager::handleTransportLost() {
  // A live MIDI link dropped mid-note: the matching Note Off will never arrive,
  // so silence everything to avoid a stuck valve/blow/pump/fan.
  // Panique a part entiere, et c'est LE chemin que le compteur existe pour
  // rendre visible : deconnexion BLE, deconnexion rtpMIDI, chute du lien
  // Wi-Fi STA, timeout Active Sensing du MIDI serie. Aucun ne passe par le
  // code web, donc aucun n'appelait requestCalibrationCancel().
  executePanic();
  if (DEBUG) {
    Serial.println("DEBUG: InstrumentManager - Transport perdu -> panic (allSoundOff)");
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
  // Remettre aussi l'etat runtime d'expression (lissage CC2, timeout CC2, mode
  // d'attaque CC73) : le manager ne detenait que les valeurs de CC.
  _airflowCtrl.resetRuntimeState();
  _cc2Pending = false;

  if (DEBUG) {
    Serial.println("DEBUG: InstrumentManager - Reset All Controllers");
  }
}

void InstrumentManager::setPWM(uint8_t channel, uint16_t on, uint16_t off) {
  // Refuse writes when the hardware is not usable (failed init), but allow the
  // safe-init sequence itself to program the PCA registers.
  if (!_initializingHardware && _hardwareInitStatus != HW_INIT_OK) return;
  // During the safe-init sequence, program the PWM registers but do NOT register
  // activity / enable OE: OE must stay HIGH until every channel holds a safe value,
  // otherwise the servos could be energised (with stale registers after a soft
  // reset) while the channels are still being initialised one by one.
  if (!_initializingHardware) registerActuatorActivity();   // differe l'OE a managePower()
  if (channel < 16) {
    _pwm0.setPWM(channel, on, off);
  } else if (_secondBoardEnabled) {
    _pwm1.setPWM(channel - 16, on, off);
  }
}

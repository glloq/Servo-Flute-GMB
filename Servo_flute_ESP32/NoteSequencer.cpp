#include "NoteSequencer.h"
#include "ConfigStorage.h"

NoteSequencer::NoteSequencer(EventQueue& eventQueue, FingerController& fingerCtrl, AirflowController& airflowCtrl)
  : _eventQueue(eventQueue), _fingerCtrl(fingerCtrl), _airflowCtrl(airflowCtrl),
    _currentState(STATE_IDLE), _currentNote(0), _currentVelocity(0),
    _stateStartTime(0), _eventScheduledTime(0), _playbackStartTime(0),
    _noteSoundStartTime(0), _pendingStopAfterMinDuration(false) {
}

void NoteSequencer::begin() {
  _currentState = STATE_IDLE;

  if (DEBUG) {
    Serial.println("DEBUG: NoteSequencer - Initialisation");
  }
}

void NoteSequencer::update() {
  processDueEvents();
  switch (_currentState) {
    case STATE_IDLE:
      // processDueEvents() ci-dessus a deja traite la file ; rien de plus a faire.
      break;
    case STATE_POSITIONING:
      handlePositioning();
      break;
    case STATE_PLAYING:
      handlePlaying();
      break;
    case STATE_STOPPING:
      handleStopping();
      break;
  }
}

NoteState NoteSequencer::getState() const {
  return _currentState;
}

bool NoteSequencer::isPlaying() const {
  return _currentState == STATE_PLAYING;
}

void NoteSequencer::handlePositioning() {
  unsigned long elapsed = millis() - _stateStartTime;

  if (elapsed >= cfg.servoToSolenoidDelayMs) {
    // Open the valve only if the note should actually sound. When CC2 (breath)
    // requests silence at the onset, setAirflowForNote() returns false and the
    // valve must stay closed instead of being force-opened.
    bool sound = _airflowCtrl.setAirflowForNote(_currentNote, _currentVelocity);
    if (sound) _airflowCtrl.openSolenoid();
    else _airflowCtrl.closeSolenoid();
    _noteSoundStartTime = millis();
    // NB: on ne remet PAS _pendingStopAfterMinDuration a false ici. Un Note Off
    // recu pendant le POSITIONING (note plus courte que la fenetre de
    // positionnement) l'a arme pour que la note sonne quand meme au moins
    // minNoteDurationMs. Il est deja remis a false a chaque nouveau Note On.
    transitionTo(STATE_PLAYING);

    if (DEBUG) {
      unsigned long actualTime = millis() - _playbackStartTime;
      unsigned long targetTime = _eventScheduledTime - _playbackStartTime;
      long timing_error = (long)actualTime - (long)targetTime;

      Serial.print("DEBUG: NoteSequencer - SON produit note ");
      Serial.print(_currentNote);
      Serial.print(" | Erreur: ");
      Serial.print(timing_error);
      Serial.println("ms");
    }
  }
}

void NoteSequencer::handlePlaying() {
  // Comparaison signee 32 bits explicite : sure au rollover de millis() et
  // identique sur l'hote des tests (ou `long` fait 64 bits) et sur l'ESP32.
  if (_pendingStopAfterMinDuration && (int32_t)(millis() - (_noteSoundStartTime + cfg.minNoteDurationMs)) >= 0) {
    _pendingStopAfterMinDuration = false;
    stopCurrentNote();
  }
}

void NoteSequencer::handleStopping() {
  transitionTo(STATE_IDLE);
}

void NoteSequencer::processDueEvents() {
  // L'epoque de la file est capturee AVANT la salve. Un clear() concurrent
  // (panic / All Sound Off / demarrage d'une session d'actionneurs) l'incremente :
  // on abandonne alors immediatement la salve pour qu'aucun evenement de l'ancien
  // contexte ne soit encore execute apres le panic.
  const uint32_t startEpoch = _eventQueue.epoch();
  // Reference de lecture capturee AVANT tout retrait : une fois la file videe,
  // getReferenceTime() repasse a 0 (elle ne sert qu'aux traces de timing).
  const unsigned long queueReference = _eventQueue.getReferenceTime();

  MidiEvent due;
  uint32_t popEpoch = startEpoch;
  // tryPopDueEvent() lit l'echeance ET retire le MEME evenement sous le MEME
  // verrou : plus aucune fenetre entre "regarder" et "consommer".
  while (_eventQueue.tryPopDueEvent(millis(), cfg.servoToSolenoidDelayMs, due, &popEpoch)) {
    if (popEpoch != startEpoch) {
      // La file a ete videe pendant la salve : l'evenement qu'on vient de retirer
      // appartient au nouveau contexte, mais la politique de panic prime. On le
      // laisse tomber et on sort ; le prochain update() repartira proprement.
      return;
    }

    if (_playbackStartTime == 0) {
      _playbackStartTime = (queueReference != 0) ? queueReference : due.timestamp;
    }

    if (due.type == EVENT_NOTE_ON) {
      // Monophonic policy: any due NOTE_ON has priority over min duration and replaces
      // the current note/positioning immediately so no stale NOTE_OFF can block the FIFO.
      _pendingStopAfterMinDuration = false;
      if (_currentState != STATE_IDLE) {
        stopCurrentNoteForReplacement();
      }
      startNoteSequence(due.midiNote, due.velocity, due.timestamp);
      continue;
    }

    if (due.type == EVENT_NOTE_OFF) {
      // Old NOTE_OFF events for notes already replaced are intentionally ignored.
      if ((_currentState == STATE_PLAYING || _currentState == STATE_POSITIONING) && due.midiNote == _currentNote) {
        if (_currentState == STATE_PLAYING) {
          unsigned long playedFor = millis() - _noteSoundStartTime;
          if (playedFor < cfg.minNoteDurationMs) {
            _pendingStopAfterMinDuration = true;
          } else {
            stopCurrentNote();
          }
        } else {
          // POSITIONING : la valve n'est pas encore ouverte. Si le Note Off est
          // reellement posterieur au Note On (vraie note courte, plus breve que
          // la fenetre de positionnement), on differe l'arret pour que la note
          // sonne quand meme minNoteDurationMs une fois lancee. Si le Note Off
          // est simultane au Note On (duree nulle/degenere), on annule direct.
          if (due.timestamp > _eventScheduledTime) {
            _pendingStopAfterMinDuration = true;
          } else {
            stopCurrentNote();
          }
        }
      }
      continue;
    }
  }
}

void NoteSequencer::startNoteSequence(byte note, byte velocity, unsigned long scheduledTime) {
  _currentNote = note;
  _currentVelocity = velocity;
  _eventScheduledTime = scheduledTime;

  _fingerCtrl.setFingerPatternForNote(note);
  transitionTo(STATE_POSITIONING);

  if (DEBUG) {
    Serial.print("DEBUG: NoteSequencer - Debut sequence note: ");
    Serial.print(note);
    Serial.print(" (vel: ");
    Serial.print(velocity);
    Serial.println(")");
  }
}

bool NoteSequencer::shouldCloseValveBetweenNotes() {
  // Modes without physical valve: no need to close
  if (cfg.airMode == AIR_MODE_SERVO_ONLY || cfg.airMode == AIR_MODE_FAN_SERVO) {
    return true;  // Will call closeValve which is a no-op, but sets rest angle
  }

  // Copie par valeur : l'evenement suivant peut etre retire ou evince par une
  // autre tache juste apres cette lecture ; on ne garde donc jamais de pointeur.
  MidiEvent nextEvent;
  if (!_eventQueue.peekCopy(nextEvent) || nextEvent.type != EVENT_NOTE_ON) {
    return true;
  }

  unsigned long currentTime = millis();
  unsigned long nextNoteTime = nextEvent.timestamp;

  if (nextNoteTime > currentTime) {
    unsigned long interval = nextNoteTime - currentTime;
    if (interval < cfg.minNoteIntervalForValveCloseMs) {
      if (DEBUG) {
        Serial.print("DEBUG: NoteSequencer - Valve GARDEE ouverte (note suivante dans ");
        Serial.print(interval);
        Serial.println("ms)");
      }
      return false;
    }
  }

  return true;
}

void NoteSequencer::stopCurrentNoteForReplacement() {
  bool closeValve = shouldCloseValveBetweenNotes();

  if (closeValve) {
    _airflowCtrl.closeSolenoid();
    _airflowCtrl.setAirflowToRest();
  } else {
    _airflowCtrl.setAirflowVelocity(1);
  }
}

void NoteSequencer::stopCurrentNote() {
  bool closeValve = shouldCloseValveBetweenNotes();

  if (closeValve) {
    _airflowCtrl.closeSolenoid();
    _airflowCtrl.setAirflowToRest();
  } else {
    _airflowCtrl.setAirflowVelocity(1);
  }

  transitionTo(STATE_STOPPING);
}

void NoteSequencer::transitionTo(NoteState newState) {
  _currentState = newState;
  _stateStartTime = millis();
}

void NoteSequencer::stop() {
  _currentNote = 0;
  _currentVelocity = 0;
  _pendingStopAfterMinDuration = false;
  _eventQueue.clear();
  _airflowCtrl.closeSolenoid();
  _airflowCtrl.setAirflowToRest();
  transitionTo(STATE_IDLE);

  if (DEBUG) {
    Serial.println("DEBUG: NoteSequencer - STOP force (All Sound Off)");
  }
}

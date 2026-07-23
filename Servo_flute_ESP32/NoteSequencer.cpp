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
      handleIdle();
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

void NoteSequencer::handleIdle() {
  processDueEvents();
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
    _pendingStopAfterMinDuration = false;
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
  if (_pendingStopAfterMinDuration && (long)(millis() - (_noteSoundStartTime + cfg.minNoteDurationMs)) >= 0) {
    _pendingStopAfterMinDuration = false;
    stopCurrentNote();
  }
}

void NoteSequencer::handleStopping() {
  transitionTo(STATE_IDLE);
}

void NoteSequencer::processDueEvents() {
  while (!_eventQueue.isEmpty()) {
    MidiEvent* event = _eventQueue.peek();
    if (event == nullptr) return;

    if (_playbackStartTime == 0) {
      _playbackStartTime = _eventQueue.getReferenceTime();
    }

    unsigned long dueTime = event->timestamp;
    if (event->type == EVENT_NOTE_ON) {
      dueTime = (event->timestamp > cfg.servoToSolenoidDelayMs) ? event->timestamp - cfg.servoToSolenoidDelayMs : 0;
    }
    if ((long)(millis() - dueTime) < 0) return;

    MidiEvent due = *event;
    _eventQueue.dequeue();

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
          stopCurrentNote();
        }
      }
      continue;
    }
  }
}

void NoteSequencer::processNextEvent() {
  processDueEvents();
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

  MidiEvent* nextEvent = _eventQueue.peek();

  if (nextEvent == nullptr || nextEvent->type != EVENT_NOTE_ON) {
    return true;
  }

  unsigned long currentTime = millis();
  unsigned long nextNoteTime = nextEvent->timestamp;

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

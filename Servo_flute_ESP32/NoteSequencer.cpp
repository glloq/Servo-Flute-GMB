#include "NoteSequencer.h"
#include "ConfigStorage.h"

NoteSequencer::NoteSequencer(EventQueue& eventQueue, FingerController& fingerCtrl, AirflowController& airflowCtrl)
  : _eventQueue(eventQueue), _fingerCtrl(fingerCtrl), _airflowCtrl(airflowCtrl),
    _currentState(STATE_IDLE), _currentNote(0), _currentVelocity(0),
    _stateStartTime(0), _eventScheduledTime(0), _playbackStartTime(0) {
}

void NoteSequencer::begin() {
  _currentState = STATE_IDLE;

  if (DEBUG) {
    Serial.println("DEBUG: NoteSequencer - Initialisation");
  }
}

void NoteSequencer::update() {
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
  if (!_eventQueue.isEmpty()) {
    processNextEvent();
  }
}

void NoteSequencer::handlePositioning() {
  unsigned long elapsed = millis() - _stateStartTime;

  if (elapsed >= cfg.servoToSolenoidDelayMs) {
    _airflowCtrl.setAirflowForNote(_currentNote, _currentVelocity);
    _airflowCtrl.openSolenoid();
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
  MidiEvent* nextEvent = _eventQueue.peek();

  if (nextEvent != nullptr && nextEvent->type == EVENT_NOTE_OFF &&
      nextEvent->midiNote == _currentNote) {
    unsigned long eventAbsoluteTime = _playbackStartTime + nextEvent->timestamp;

    if (millis() >= eventAbsoluteTime) {
      _eventQueue.dequeue();
      stopCurrentNote();
    }
  }
}

void NoteSequencer::handleStopping() {
  transitionTo(STATE_IDLE);
}

void NoteSequencer::processNextEvent() {
  MidiEvent* event = _eventQueue.peek();

  if (event == nullptr) {
    return;
  }

  unsigned long eventAbsoluteTime = _playbackStartTime + event->timestamp;

  if (_playbackStartTime == 0) {
    _playbackStartTime = millis();
    eventAbsoluteTime = _playbackStartTime + event->timestamp;
  }

  const unsigned long MECHANICAL_DELAY = cfg.servoToSolenoidDelayMs;

  unsigned long startTime;
  if (event->type == EVENT_NOTE_ON) {
    if (eventAbsoluteTime > MECHANICAL_DELAY) {
      startTime = eventAbsoluteTime - MECHANICAL_DELAY;
    } else {
      startTime = 0;
    }
  } else {
    startTime = eventAbsoluteTime;
  }

  if (millis() >= startTime) {
    if (event->type == EVENT_NOTE_ON) {
      if (_currentState != STATE_IDLE) {
        stopCurrentNote();
      }
      startNoteSequence(event->midiNote, event->velocity, eventAbsoluteTime);
      _eventQueue.dequeue();

    } else if (event->type == EVENT_NOTE_OFF) {
      if (_currentState == STATE_PLAYING && event->midiNote == _currentNote) {
        stopCurrentNote();
      }
      _eventQueue.dequeue();
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
  MidiEvent* nextEvent = _eventQueue.peek();

  if (nextEvent == nullptr || nextEvent->type != EVENT_NOTE_ON) {
    return true;
  }

  unsigned long currentTime = millis();
  unsigned long nextNoteTime = _playbackStartTime + nextEvent->timestamp;

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
  transitionTo(STATE_IDLE);

  if (DEBUG) {
    Serial.println("DEBUG: NoteSequencer - STOP force (All Sound Off)");
  }
}

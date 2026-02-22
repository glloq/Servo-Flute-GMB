#include "AutoCalibrator.h"

#if MIC_ENABLED

#include "FingerController.h"
#include "AirflowController.h"
#include "AudioAnalyzer.h"
#include "ConfigStorage.h"

AutoCalibrator::AutoCalibrator(FingerController& fingers, AirflowController& airflow, AudioAnalyzer& audio)
  : _fingers(fingers), _airflow(airflow), _audio(audio),
    _state(ACAL_IDLE), _mode(ACAL_MODE_AIRFLOW),
    _currentNote(0), _currentAngle(0), _stateTimer(0),
    _foundMin(false), _airMinPct(0), _airMaxPct(0), _silenceCounter(0) {
  memset(_results, 0, sizeof(_results));
}

void AutoCalibrator::start(AutoCalMode mode) {
  _mode = mode;
  _currentNote = 0;
  memset(_results, 0, sizeof(_results));

  // Ensure audio analyzer is active
  _audio.setActive(true);

  // Begin with first note
  _state = ACAL_PREPARE;
  _stateTimer = millis();

  if (DEBUG) {
    Serial.println("DEBUG: AutoCalibrator - Demarrage calibration airflow");
  }
}

void AutoCalibrator::stop() {
  _airflow.testSolenoid(false);
  _airflow.setAirflowToRest();
  _fingers.closeAllFingers();
  _state = ACAL_IDLE;

  if (DEBUG) {
    Serial.println("DEBUG: AutoCalibrator - Arret");
  }
}

void AutoCalibrator::update() {
  if (_state == ACAL_IDLE || _state == ACAL_COMPLETE) return;

  unsigned long now = millis();
  unsigned long elapsed = now - _stateTimer;

  switch (_state) {
    case ACAL_PREPARE: {
      // Position fingers for current note (from runtime config)
      byte midi = cfg.notes[_currentNote].midiNote;
      _fingers.setFingerPatternForNote(midi);

      // Set airflow to off position (start of sweep)
      _currentAngle = cfg.servoAirflowOff;
      _airflow.testAirflowAngle(_currentAngle);

      // Open solenoid
      _airflow.testSolenoid(true);

      _foundMin = false;
      _airMinPct = 0;
      _airMaxPct = 100;
      _silenceCounter = 0;

      _state = ACAL_SETTLE;
      _stateTimer = now;

      if (DEBUG) {
        Serial.print("DEBUG: AutoCal - Note ");
        Serial.print(_currentNote);
        Serial.print(" MIDI ");
        Serial.println(midi);
      }
      break;
    }

    case ACAL_SETTLE: {
      // Wait for servos to reach position
      if (elapsed >= AUTOCAL_SETTLE_MS) {
        _state = ACAL_SWEEP;
        _stateTimer = now;
      }
      break;
    }

    case ACAL_SWEEP: {
      if (elapsed < AUTOCAL_STEP_MS) return;
      _stateTimer = now;

      // Increment airflow angle
      _currentAngle++;

      if (_currentAngle > (int)cfg.servoAirflowMax + AUTOCAL_SWEEP_OVERSHOOT) {
        // Reached max angle - note done
        if (_foundMin) {
          _airMaxPct = 100;
        }
        _state = ACAL_NOTE_DONE;
        _stateTimer = now;
        break;
      }

      _airflow.testAirflowAngle(_currentAngle);

      // Analyze audio
      bool soundNow = _audio.isSoundDetected();
      int detectedMidi = _audio.getPitchMidi();
      byte expectedMidi = cfg.notes[_currentNote].midiNote;

      // Check if pitch is close to expected (within tolerance)
      bool pitchOk = (detectedMidi > 0) &&
                     (abs(detectedMidi - expectedMidi) <= AUTOCAL_PITCH_TOLERANCE_SEMI);

      if (!_foundMin) {
        // Looking for sound onset
        if (soundNow && pitchOk) {
          _foundMin = true;
          _airMinPct = angleToPct(_currentAngle);
          if (_airMinPct < 0) _airMinPct = 0;
          _silenceCounter = 0;

          if (DEBUG) {
            Serial.print("DEBUG: AutoCal - air_min found at angle ");
            Serial.print(_currentAngle);
            Serial.print(" = ");
            Serial.print(_airMinPct);
            Serial.println("%");
          }
        }
      } else {
        // Sound found - looking for offset or overblow
        if (!soundNow || !pitchOk) {
          _silenceCounter++;
          if (_silenceCounter >= AUTOCAL_SILENCE_COUNT) {
            // Sound has stopped or pitch went wrong
            _airMaxPct = angleToPct(_currentAngle - AUTOCAL_SILENCE_COUNT);
            if (_airMaxPct > 100) _airMaxPct = 100;
            if (_airMaxPct < _airMinPct) _airMaxPct = _airMinPct + AUTOCAL_MIN_RANGE_PCT;

            if (DEBUG) {
              Serial.print("DEBUG: AutoCal - air_max found at ");
              Serial.print(_airMaxPct);
              Serial.println("%");
            }

            _state = ACAL_NOTE_DONE;
            _stateTimer = now;
          }
        } else {
          _silenceCounter = 0;  // Reset if sound is back
        }
      }
      break;
    }

    case ACAL_NOTE_DONE: {
      // First entry: close solenoid and store result
      if (elapsed < AUTOCAL_STORE_DELAY_MS) {
        _airflow.testSolenoid(false);
        _airflow.setAirflowToRest();

        // Store result
        _results[_currentNote].valid = _foundMin;
        _results[_currentNote].airMin = (uint8_t)constrain(_airMinPct, 0, 100);
        _results[_currentNote].airMax = (uint8_t)constrain(_airMaxPct, 0, 100);

        if (DEBUG) {
          Serial.print("DEBUG: AutoCal - Note ");
          Serial.print(_currentNote);
          Serial.print(" result: ");
          Serial.print(_foundMin ? "OK" : "FAIL");
          Serial.print(" min=");
          Serial.print(_airMinPct);
          Serial.print("% max=");
          Serial.print(_airMaxPct);
          Serial.println("%");
        }
      }

      // Wait briefly before next note
      if (elapsed >= AUTOCAL_NOTE_INTERVAL_MS) {
        advanceToNextNote();
      }
      break;
    }

    default:
      break;
  }
}

int AutoCalibrator::angleToPct(int angle) {
  int range = (int)cfg.servoAirflowMax - (int)cfg.servoAirflowMin;
  if (range <= 0) return 0;
  return ((angle - (int)cfg.servoAirflowMin) * 100) / range;
}

void AutoCalibrator::advanceToNextNote() {
  _currentNote++;
  if (_currentNote >= cfg.numNotes) {
    _state = ACAL_COMPLETE;
    _airflow.testSolenoid(false);
    _airflow.setAirflowToRest();
    _fingers.closeAllFingers();

    if (DEBUG) {
      Serial.println("DEBUG: AutoCalibrator - Calibration terminee");
    }
  } else {
    _state = ACAL_PREPARE;
    _stateTimer = millis();
  }
}

void AutoCalibrator::applyResults() {
  for (int i = 0; i < cfg.numNotes; i++) {
    if (_results[i].valid) {
      cfg.notes[i].airflowMinPercent = _results[i].airMin;
      cfg.notes[i].airflowMaxPercent = _results[i].airMax;
    }
  }
  ConfigStorage::save();

  if (DEBUG) {
    Serial.println("DEBUG: AutoCalibrator - Resultats appliques et sauvegardes");
  }
}

#endif // MIC_ENABLED

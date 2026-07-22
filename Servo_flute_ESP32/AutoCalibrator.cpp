#include "AutoCalibrator.h"

#if MIC_ENABLED

#include <math.h>
#include "FingerController.h"
#include "AirflowController.h"
#include "IAudioSource.h"
#include "ConfigStorage.h"
#include "PitchMath.h"

using AutoCalMath::AudioFrame;
using AutoCalMath::PositionStats;

// Cents spread mapped to zero stability. Two stable-tolerance widths of wobble is
// treated as "no stability left".
static const float kStabilityRefCents = 2.0f * AUTOCAL_PITCH_TOLERANCE_STABLE_CENTS;

AutoCalibrator::AutoCalibrator(FingerController& fingers, AirflowController& airflow, IAudioSource& audio)
  : _fingers(fingers), _airflow(airflow), _audio(audio),
    _state(ACAL_IDLE), _mode(ACAL_MODE_AIRFLOW), _startTime(0), _timeoutEvent(false),
    _numNotes(0), _currentNote(0), _expectedMidi(0),
    _phase(PH_PREPARE), _step(ST_SET), _stateTimer(0), _lastFrameTime(0), _stepPercent(0),
    _frameCount(0), _lastMeanRms(0), _lastValidFrames(0), _lastTotalFrames(0),
    _lastStability(0), _lastSnr(0), _lastMeanConf(0), _lastValidRatio(0), _lastConfidencePct(0),
    _dispHz(0), _dispMidi(0), _dispCents(0),
    _noiseRms(0), _soundThreshold(MIC_RMS_ABSOLUTE_MIN), _noiseAccum(0), _noiseCount(0), _noiseSettled(false),
    _coarseMin(-1), _coarseMax(-1), _coarseValidSeen(false), _coarseLossCount(0),
    _airMin(-1), _airMax(-1), _fineLastValid(-1), _fineLossCount(0),
    _nominalCount(0), _nominalIndex(0),
    _bestValidFound(false), _bestPercent(-1), _bestScore(-1), _bestCents(0), _bestStability(0), _bestSnr(0), _bestConfPct(0),
    _rfStep(RF_PREPARE), _currentAngle(0), _rfMinAngle(-1), _rfMaxAngle(-1), _rfFoundMin(false), _rfSilenceCount(0) {
  memset(_results, 0, sizeof(_results));
  memset(_nominalCandidates, 0, sizeof(_nominalCandidates));
  memset(_frames, 0, sizeof(_frames));
}

void AutoCalibrator::start(AutoCalMode mode) {
  if (isRunning()) stop();

  // Refuse to run without a working microphone - leave hardware safe.
  if (!_audio.isMicDetected()) {
    safeHardware();
    _state = ACAL_IDLE;
    if (DEBUG) Serial.println("DEBUG: AutoCalibrator - Pas de micro, calibration ignoree");
    return;
  }

  _mode = mode;
  _currentNote = 0;
  _numNotes = cfg.numNotes;
  _startTime = millis();
  _timeoutEvent = false;
  memset(_results, 0, sizeof(_results));
  _audio.setActive(true);

  if (mode == ACAL_MODE_RANGE_FIND) {
    _state = ACAL_RANGE;
    _rfStep = RF_PREPARE;
    _rfMinAngle = -1;
    _rfMaxAngle = -1;
    _rfFoundMin = false;
    _rfSilenceCount = 0;
    _currentAngle = 0;
    _stateTimer = _startTime;
    if (DEBUG) Serial.println("DEBUG: AutoCalibrator - Demarrage range finder");
  } else {
    _state = ACAL_AIRFLOW;
    _phase = PH_PREPARE;
    _stateTimer = _startTime;
    if (DEBUG) Serial.println("DEBUG: AutoCalibrator - Demarrage calibration airflow");
  }
}

void AutoCalibrator::safeHardware() {
  _airflow.testSolenoid(false);
  _airflow.setAirflowToRest();
  _fingers.closeAllFingers();
}

void AutoCalibrator::stop() {
  safeHardware();
  _state = ACAL_IDLE;
  if (DEBUG) Serial.println("DEBUG: AutoCalibrator - Arret");
}

void AutoCalibrator::abortTimeout() {
  safeHardware();
  _state = ACAL_IDLE;
  _timeoutEvent = true;
  if (DEBUG) Serial.println("ERREUR: AutoCalibrator - Timeout global, arret securise");
}

void AutoCalibrator::update() {
  if (!isRunning()) return;

  unsigned long now = millis();
  if (now - _startTime >= AUTOCAL_GLOBAL_TIMEOUT_MS) {
    abortTimeout();
    return;
  }

  if (_mode == ACAL_MODE_RANGE_FIND) {
    updateRangeFinder(now);
  } else {
    updateAirflow(now);
  }
}

const char* AutoCalibrator::getPhaseName() const {
  if (_mode == ACAL_MODE_RANGE_FIND) return "range";
  switch (_phase) {
    case PH_PREPARE:  return "prepare";
    case PH_NOISE:    return "noise";
    case PH_COARSE:   return "coarse";
    case PH_FINE_MIN: return "fine";
    case PH_FINE_MAX: return "fine";
    case PH_NOMINAL:  return "nominal";
    case PH_FINALIZE: return "done";
  }
  return "?";
}

float AutoCalibrator::currentToleranceCents() const {
  // Wide tolerance only to detect the note's first appearance; strict tolerance
  // to define the stable plateau and the nominal value.
  if (_phase == PH_COARSE || _phase == PH_FINE_MIN) return AUTOCAL_PITCH_TOLERANCE_ONSET_CENTS;
  return AUTOCAL_PITCH_TOLERANCE_STABLE_CENTS;
}

void AutoCalibrator::setAirflowPercent(int percent) {
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  int range = (int)cfg.servoAirflowMax - (int)cfg.servoAirflowMin;
  if (range < 0) range = 0;
  int angle = (int)cfg.servoAirflowMin + (range * percent) / 100;
  _currentAngle = angle;
  _airflow.testAirflowAngle((uint16_t)angle);
}

// ------------------------------------------------------------------ airflow ----

void AutoCalibrator::updateAirflow(unsigned long now) {
  switch (_phase) {
    case PH_PREPARE:
      prepareNote(now);
      break;
    case PH_NOISE:
      runNoise(now);
      break;
    case PH_COARSE:
    case PH_FINE_MIN:
    case PH_FINE_MAX:
    case PH_NOMINAL:
      if (runPositionStep(now)) onPositionEvaluated(now);
      break;
    case PH_FINALIZE:
      finalizeNote(now);
      break;
  }
}

void AutoCalibrator::prepareNote(unsigned long now) {
  _expectedMidi = cfg.notes[_currentNote].midiNote;
  _fingers.setFingerPatternForNote(_expectedMidi);
  // Noise floor is measured with the valve closed and the air at rest, fingers
  // already positioned, no mechanical action in progress.
  _airflow.testSolenoid(false);
  _airflow.setAirflowToRest();
  _stepPercent = 0;
  _currentAngle = cfg.servoAirflowOff;

  // reset per-note tracking
  _coarseMin = _coarseMax = -1;
  _coarseValidSeen = false;
  _coarseLossCount = 0;
  _airMin = _airMax = -1;
  _fineLastValid = -1;
  _fineLossCount = 0;
  _bestValidFound = false;
  _bestScore = -1;
  _bestPercent = -1;
  _bestCents = _bestStability = _bestSnr = 0;
  _bestConfPct = 0;
  _nominalCount = 0;
  _nominalIndex = 0;
  _noiseAccum = 0;
  _noiseCount = 0;
  _noiseSettled = false;
  _noiseRms = 0;

  _phase = PH_NOISE;
  _stateTimer = now;
  _lastFrameTime = now;

  if (DEBUG) {
    Serial.print("DEBUG: AutoCal - Note ");
    Serial.print(_currentNote);
    Serial.print(" MIDI ");
    Serial.println(_expectedMidi);
  }
}

void AutoCalibrator::runNoise(unsigned long now) {
  // Let the finger servos settle before sampling the noise floor.
  if (!_noiseSettled) {
    if (now - _stateTimer >= AUTOCAL_AIR_SETTLE_MS) {
      _noiseSettled = true;
      _stateTimer = now;
      _lastFrameTime = now;
      _noiseAccum = 0;
      _noiseCount = 0;
    }
    return;
  }

  if (now - _lastFrameTime >= AUTOCAL_FRAME_SAMPLE_MS) {
    _lastFrameTime = now;
    _noiseAccum += _audio.getRMS();
    _noiseCount++;
  }

  if (now - _stateTimer >= AUTOCAL_NOISE_MEASURE_MS) {
    _noiseRms = (_noiseCount > 0) ? (_noiseAccum / (float)_noiseCount) : 0.0f;
    _soundThreshold = AutoCalMath::computeSoundThreshold(_noiseRms);
    // Open the air path and start the coarse sweep at 0 %.
    _airflow.testSolenoid(true);
    _phase = PH_COARSE;
    beginPosition(0, now);

    if (DEBUG) {
      Serial.print("DEBUG: AutoCal - noiseRms ");
      Serial.print(_noiseRms, 4);
      Serial.print(" threshold ");
      Serial.println(_soundThreshold, 4);
    }
  }
}

void AutoCalibrator::beginPosition(int percent, unsigned long now) {
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  _stepPercent = percent;
  _step = ST_SET;
  _frameCount = 0;
  _stateTimer = now;
}

void AutoCalibrator::sampleFrame(AudioFrame& f) {
  f.pitchValid = _audio.isPitchValid();
  f.rms = _audio.getRMS();
  f.hz = _audio.getPitchHz();
  f.midi = _audio.getPitchMidi();
  f.cents = _audio.getPitchCents();
  f.confidence = _audio.getPitchConfidence();
}

bool AutoCalibrator::runPositionStep(unsigned long now) {
  switch (_step) {
    case ST_SET:
      // SET_AIRFLOW: move the servo, then wait for it to settle.
      setAirflowPercent(_stepPercent);
      _stateTimer = now;
      _step = ST_SETTLE;
      return false;

    case ST_SETTLE:
      // WAIT_AIRFLOW_SETTLE: ignore the transient frames during servo travel.
      if (now - _stateTimer >= AUTOCAL_AIR_SETTLE_MS) {
        _frameCount = 0;
        _lastFrameTime = now;
        _step = ST_COLLECT;
      }
      return false;

    case ST_COLLECT:
      // COLLECT_AUDIO: gather several spaced frames.
      if (now - _lastFrameTime >= AUTOCAL_FRAME_SAMPLE_MS) {
        _lastFrameTime = now;
        sampleFrame(_frames[_frameCount]);
        _frameCount++;
        if (_frameCount >= AUTOCAL_AUDIO_FRAMES_PER_STEP) _step = ST_EVAL;
      }
      return false;

    case ST_EVAL: {
      // EVALUATE_AUDIO: aggregate frames into a position decision + metrics.
      float tol = currentToleranceCents();
      _lastStats = AutoCalMath::evaluatePosition(_frames, _frameCount, _expectedMidi,
                                                 _soundThreshold, tol, MIC_YIN_CONFIDENCE_MIN);
      _lastMeanRms = _lastStats.meanRms;
      _lastValidFrames = _lastStats.validFrames;
      _lastTotalFrames = _lastStats.totalFrames;
      _lastValidRatio = (_lastStats.totalFrames > 0)
                          ? (float)_lastStats.validFrames / (float)_lastStats.totalFrames : 0.0f;
      _lastStability = AutoCalMath::pitchStability(_lastStats.centsSpread, kStabilityRefCents);
      _lastSnr = AutoCalMath::snrDb(_lastStats.meanRms, _noiseRms);

      // Mean YIN confidence over matching frames (fallback: valid-pitch frames).
      float cSum = 0.0f; int cN = 0;
      for (int i = 0; i < _frameCount; i++) {
        if (AutoCalMath::frameIsNote(_frames[i], _expectedMidi, _soundThreshold, tol, MIC_YIN_CONFIDENCE_MIN)) {
          cSum += _frames[i].confidence; cN++;
        }
      }
      if (cN == 0) {
        for (int i = 0; i < _frameCount; i++) if (_frames[i].pitchValid) { cSum += _frames[i].confidence; cN++; }
      }
      _lastMeanConf = (cN > 0) ? (cSum / (float)cN) : 0.0f;

      _lastConfidencePct = AutoCalMath::computeConfidence(
          fabsf(_lastStats.medianCents), _lastStability, _lastSnr, _lastMeanConf,
          _lastValidRatio, AUTOCAL_PITCH_TOLERANCE_STABLE_CENTS);

      // Representative pitch for the UI: median of valid frames, else last frame.
      if (_lastStats.validFrames > 0) {
        _dispHz = _lastStats.medianHz;
        _dispMidi = PitchMath::hzToMidi(_lastStats.medianHz);
        _dispCents = _lastStats.medianCents;
      } else if (_frameCount > 0) {
        _dispHz = _frames[_frameCount - 1].hz;
        _dispMidi = _frames[_frameCount - 1].midi;
        _dispCents = _frames[_frameCount - 1].cents;
      } else {
        _dispHz = 0; _dispMidi = 0; _dispCents = 0;
      }
      return true;
    }
  }
  return false;
}

void AutoCalibrator::onPositionEvaluated(unsigned long now) {
  const bool valid = AutoCalMath::positionValid(_lastStats, AUTOCAL_REQUIRED_VALID_FRAMES);
  const bool over  = AutoCalMath::positionOverblown(_lastStats);

  switch (_phase) {
    case PH_COARSE: {
      if (!_coarseValidSeen) {
        if (valid && !over) {
          _coarseValidSeen = true;
          _coarseMin = _stepPercent;
          _coarseLossCount = 0;
        }
      } else {
        if (valid && !over) {
          _coarseMax = _stepPercent;
          _coarseLossCount = 0;
        } else {
          _coarseLossCount++;
          // An octave above after a valid plateau is a hard overblow limit.
          if (over || _coarseLossCount >= AUTOCAL_LOSS_CONFIRM_STEPS) {
            _phase = PH_FINE_MIN;
            int start = _coarseMin - AUTOCAL_FINE_MARGIN_PERCENT;
            beginPosition(start, now);
            return;
          }
        }
      }
      int next = _stepPercent + AUTOCAL_COARSE_STEP_PERCENT;
      if (next > 100) {
        if (!_coarseValidSeen) {
          _phase = PH_FINALIZE;   // never sounded -> failed note
        } else {
          if (_coarseMax < 0) _coarseMax = 100;   // still valid at the top
          _phase = PH_FINE_MIN;
          beginPosition(_coarseMin - AUTOCAL_FINE_MARGIN_PERCENT, now);
        }
        return;
      }
      beginPosition(next, now);
      break;
    }

    case PH_FINE_MIN: {
      if (valid && !over) {
        _airMin = _stepPercent;
        _phase = PH_FINE_MAX;
        int start = _coarseMax - AUTOCAL_FINE_MARGIN_PERCENT;
        if (start < _airMin) start = _airMin;
        beginPosition(start, now);
        return;
      }
      int next = _stepPercent + AUTOCAL_FINE_STEP_PERCENT;
      int limit = _coarseMin + AUTOCAL_COARSE_STEP_PERCENT;
      if (next > limit || next > 100) {
        _airMin = _coarseMin;   // fall back to the coarse estimate
        _phase = PH_FINE_MAX;
        int start = _coarseMax - AUTOCAL_FINE_MARGIN_PERCENT;
        if (start < _airMin) start = _airMin;
        beginPosition(start, now);
        return;
      }
      beginPosition(next, now);
      break;
    }

    case PH_FINE_MAX: {
      if (valid && !over) {
        _fineLastValid = _stepPercent;
        _fineLossCount = 0;
      } else {
        _fineLossCount++;
        if (over || _fineLossCount >= AUTOCAL_LOSS_CONFIRM_STEPS) {
          _airMax = (_fineLastValid >= 0) ? _fineLastValid : _coarseMax;
          _phase = PH_NOMINAL;
          _nominalIndex = 0;
          onPositionEvaluated(now);   // build candidates and start (see PH_NOMINAL init below)
          return;
        }
      }
      int next = _stepPercent + AUTOCAL_FINE_STEP_PERCENT;
      int limit = _coarseMax + 2 * AUTOCAL_FINE_MARGIN_PERCENT;
      if (limit > 100) limit = 100;
      if (next > limit) {
        _airMax = (_fineLastValid >= 0) ? _fineLastValid : ((_coarseMax >= 0) ? _coarseMax : _airMin);
        _phase = PH_NOMINAL;
        _nominalIndex = 0;
        onPositionEvaluated(now);
        return;
      }
      beginPosition(next, now);
      break;
    }

    case PH_NOMINAL: {
      // On first entry (_nominalIndex==0 and candidate list empty) build the
      // candidate set spread across the stable band, then evaluate them one by one.
      if (_nominalIndex == 0 && _nominalCount == 0) {
        if (_airMin < 0) _airMin = _coarseMin;
        if (_airMax < _airMin) _airMax = _airMin;
        int span = _airMax - _airMin;
        _nominalCount = 0;
        // fractions 0.20, 0.35, 0.50, 0.65, 0.80 of the stable band
        static const float fr[5] = {0.20f, 0.35f, 0.50f, 0.65f, 0.80f};
        for (int k = 0; k < 5; k++) {
          int p = _airMin + (int)lroundf(fr[k] * (float)span);
          if (p < _airMin) p = _airMin;
          if (p > _airMax) p = _airMax;
          // de-duplicate consecutive equal candidates (narrow bands)
          if (_nominalCount == 0 || _nominalCandidates[_nominalCount - 1] != (uint8_t)p) {
            _nominalCandidates[_nominalCount++] = (uint8_t)p;
          }
        }
        beginPosition(_nominalCandidates[0], now);
        return;
      }

      // Score the just-evaluated candidate.
      if (valid && !over) {
        float absCents = fabsf(_lastStats.medianCents);
        float quality = AutoCalMath::positionQuality(absCents, _lastStability, _lastSnr,
                                                     _lastMeanConf, AUTOCAL_PITCH_TOLERANCE_STABLE_CENTS);
        int span = _airMax - _airMin;
        int distLow = _stepPercent - _airMin;
        int distHigh = _airMax - _stepPercent;
        int dist = (distLow < distHigh) ? distLow : distHigh;
        float denom = (span > 4) ? (float)(span / 4) : 1.0f;
        float marginScore = AutoCalMath::clamp01((float)dist / denom);
        float total = 0.85f * quality + 0.15f * marginScore;
        if (!_bestValidFound || total > _bestScore) {
          _bestValidFound = true;
          _bestScore = total;
          _bestPercent = _stepPercent;
          _bestCents = _lastStats.medianCents;
          _bestStability = _lastStability;
          _bestSnr = _lastSnr;
          _bestConfPct = _lastConfidencePct;
        }
      }

      _nominalIndex++;
      if (_nominalIndex >= _nominalCount) {
        _phase = PH_FINALIZE;
        return;
      }
      beginPosition(_nominalCandidates[_nominalIndex], now);
      break;
    }

    default:
      break;
  }
}

void AutoCalibrator::finalizeNote(unsigned long now) {
  AutoCalNoteResult& r = _results[_currentNote];
  memset(&r, 0, sizeof(r));

  bool ok = _coarseValidSeen && _airMin >= 0 && _airMax >= 0 && _airMax >= _airMin;
  if (ok) {
    uint8_t mn = (uint8_t)((_airMin < 0) ? 0 : (_airMin > 100 ? 100 : _airMin));
    uint8_t mx = (uint8_t)((_airMax < 0) ? 0 : (_airMax > 100 ? 100 : _airMax));
    if (mx < mn) mx = mn;

    // Default nominal = min + fraction*(max-min); override with the best-scoring point.
    uint8_t nominal = AutoCalMath::computeNominalPercent(mn, mx, AUTOCAL_NOMINAL_FRACTION);
    float cents = 0, stability = 1.0f, snr = 0; uint8_t conf = 0;
    if (_bestValidFound && _bestPercent >= 0) {
      nominal = (uint8_t)_bestPercent;
      cents = _bestCents;
      stability = _bestStability;
      snr = _bestSnr;
      conf = _bestConfPct;
    }
    nominal = AutoCalMath::clampNominal(mn, mx, nominal, AUTOCAL_NOMINAL_GUARD_PCT);

    r.valid = true;
    r.airMin = mn;
    r.airMax = mx;
    r.airNominal = nominal;
    r.medianCents = cents;
    r.pitchStability = stability;
    r.signalToNoiseRatio = snr;
    r.confidence = conf;
  } else {
    r.valid = false;
  }

  if (DEBUG) {
    Serial.print("DEBUG: AutoCal - Note ");
    Serial.print(_currentNote);
    Serial.print(ok ? " OK min=" : " FAIL min=");
    Serial.print(r.airMin);
    Serial.print(" nom=");
    Serial.print(r.airNominal);
    Serial.print(" max=");
    Serial.print(r.airMax);
    Serial.print(" conf=");
    Serial.println(r.confidence);
  }

  // Valve closed, air at rest between notes.
  _airflow.testSolenoid(false);
  _airflow.setAirflowToRest();
  advanceNote(now);
}

void AutoCalibrator::advanceNote(unsigned long now) {
  _currentNote++;
  _nominalCount = 0;
  _nominalIndex = 0;
  if (_currentNote >= _numNotes) {
    safeHardware();
    _state = ACAL_COMPLETE;
    if (DEBUG) Serial.println("DEBUG: AutoCalibrator - Calibration terminee");
  } else {
    _phase = PH_PREPARE;
    _stateTimer = now;
  }
}

// --------------------------------------------------------------- range finder --

void AutoCalibrator::updateRangeFinder(unsigned long now) {
  switch (_rfStep) {
    case RF_PREPARE: {
      int midIdx = cfg.numNotes / 2;
      _currentNote = midIdx;
      _expectedMidi = cfg.notes[midIdx].midiNote;
      _fingers.setFingerPatternForNote(_expectedMidi);
      _currentAngle = 0;
      _airflow.testAirflowAngle(0);
      _airflow.testSolenoid(true);
      _rfFoundMin = false;
      _rfMinAngle = -1;
      _rfMaxAngle = -1;
      _rfSilenceCount = 0;
      _rfStep = RF_SETTLE;
      _stateTimer = now;
      break;
    }

    case RF_SETTLE:
      if (now - _stateTimer >= AUTOCAL_SETTLE_MS) {
        _rfStep = RF_SWEEP;
        _stateTimer = now;
      }
      break;

    case RF_SWEEP: {
      if (now - _stateTimer < AUTOCAL_RF_STEP_MS) return;
      _stateTimer = now;
      _currentAngle++;

      if (_currentAngle > 180) {
        if (_rfFoundMin && _rfMaxAngle < 0) _rfMaxAngle = 180;
        safeHardware();
        _state = ACAL_RF_COMPLETE;
        if (DEBUG) {
          Serial.print("DEBUG: RangeFinder - Complete min=");
          Serial.print(_rfMinAngle);
          Serial.print(" max=");
          Serial.println(_rfMaxAngle);
        }
        return;
      }

      _airflow.testAirflowAngle((uint16_t)_currentAngle);

      // Accept only the exact expected note with a trustworthy pitch.
      bool ok = _audio.isPitchValid() &&
                _audio.getPitchMidi() == (int)_expectedMidi &&
                _audio.getRMS() > MIC_RMS_ABSOLUTE_MIN;

      if (!_rfFoundMin) {
        if (ok) {
          _rfFoundMin = true;
          _rfMinAngle = _currentAngle - AUTOCAL_RF_MARGIN_DEG;
          if (_rfMinAngle < 0) _rfMinAngle = 0;
          _rfSilenceCount = 0;
        }
      } else {
        if (!ok) {
          _rfSilenceCount++;
          if (_rfSilenceCount >= AUTOCAL_SILENCE_COUNT) {
            _rfMaxAngle = _currentAngle - AUTOCAL_SILENCE_COUNT + AUTOCAL_RF_MARGIN_DEG;
            if (_rfMaxAngle > 180) _rfMaxAngle = 180;
            safeHardware();
            _state = ACAL_RF_COMPLETE;
            if (DEBUG) {
              Serial.print("DEBUG: RangeFinder - max at ");
              Serial.println(_rfMaxAngle);
            }
          }
        } else {
          _rfSilenceCount = 0;
        }
      }
      break;
    }
  }
}

// ----------------------------------------------------------------- persistence -

void AutoCalibrator::applyResults() {
  // Never overwrite a previously valid calibration with a failed new one.
  for (int i = 0; i < cfg.numNotes && i < _numNotes; i++) {
    if (_results[i].valid) {
      cfg.notes[i].airflowMinPercent = _results[i].airMin;
      cfg.notes[i].airflowMaxPercent = _results[i].airMax;
      cfg.notes[i].airflowNominalPercent = _results[i].airNominal;
    }
  }
  ConfigStorage::save();
  if (DEBUG) Serial.println("DEBUG: AutoCalibrator - Resultats appliques et sauvegardes");
}

void AutoCalibrator::applyRangeResults() {
  if (_rfMinAngle >= 0) cfg.servoAirflowMin = _rfMinAngle;
  if (_rfMaxAngle >= 0) cfg.servoAirflowMax = _rfMaxAngle;
  ConfigStorage::save();
  if (DEBUG) {
    Serial.print("DEBUG: RangeFinder - Applique min=");
    Serial.print(cfg.servoAirflowMin);
    Serial.print(" max=");
    Serial.println(cfg.servoAirflowMax);
  }
}

#endif // MIC_ENABLED

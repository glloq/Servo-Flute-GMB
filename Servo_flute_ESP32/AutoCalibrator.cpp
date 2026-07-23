#include "AutoCalibrator.h"

#if MIC_ENABLED

#include <math.h>
#include "FingerController.h"
#include "AirflowController.h"
#include "IAudioSource.h"
#include "ICalibrationAirSupply.h"
#include "ConfigStorage.h"
#include "PitchMath.h"

using AutoCalMath::AudioFrame;
using AutoCalMath::PositionStats;

// Cents spread mapped to zero stability. Two stable-tolerance widths of wobble is
// treated as "no stability left".
static const float kStabilityRefCents = 2.0f * AUTOCAL_PITCH_TOLERANCE_STABLE_CENTS;

AutoCalibrator::AutoCalibrator(FingerController& fingers, AirflowController& airflow, IAudioSource& audio,
                               ICalibrationAirSupply& airSupply)
  : _fingers(fingers), _airflow(airflow), _audio(audio), _airSupply(airSupply),
    _airReady(false), _airPrepareTime(0), _lastAirError(CAL_AIR_OK),
    _state(ACAL_IDLE), _mode(ACAL_MODE_AIRFLOW), _startTime(0), _globalTimeout(AUTOCAL_GLOBAL_TIMEOUT_MS), _timeoutEvent(false),
    _numNotes(0), _currentNote(0), _expectedMidi(0),
    _phase(PH_PREPARE), _step(ST_SET), _stateTimer(0), _lastFrameTime(0), _stepPercent(0),
    _stepIsAngle(false), _stepAngle(0),
    _frameCount(0), _lastCollectedSeq(0), _haveCollectedSeq(false), _collectStartTime(0), _audioStale(false),
    _noteStartTime(0), _noteFailReason(ACAL_FAIL_NONE),
    _lastMeanRms(0), _lastValidFrames(0), _lastTotalFrames(0),
    _lastStability(0), _lastSnr(0), _lastMeanConf(0), _lastValidRatio(0), _lastConfidencePct(0),
    _dispHz(0), _dispMidi(0), _dispCents(0),
    _noiseRms(0), _soundThreshold(MIC_RMS_ABSOLUTE_MIN), _noiseAccum(0), _noiseCount(0), _noiseSettled(false),
    _noiseLastSeq(0), _noiseHaveSeq(false), _noiseCollectStart(0),
    _coarseMin(-1), _coarseMax(-1), _coarseValidSeen(false), _coarseLossCount(0),
    _airMin(-1), _airMax(-1), _fineLastValid(-1), _fineLossCount(0),
    _nominalCount(0), _nominalIndex(0),
    _bestValidFound(false), _bestPercent(-1), _bestScore(-1), _bestCents(0), _bestStability(0), _bestSnr(0), _bestConfPct(0),
    _currentAngle(0), _rfMinAngle(-1), _rfMaxAngle(-1), _rfFoundMin(false), _rfLossCount(0),
    _rfFailReason(ACAL_FAIL_NONE) {
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
  // Global timeout scales with the note count, capped by the absolute ceiling so
  // it stays a true last-resort safety net rather than the primary limit.
  {
    unsigned long computed = (unsigned long)_numNotes * AUTOCAL_TIMEOUT_PER_NOTE_MS + AUTOCAL_GLOBAL_MARGIN_MS;
    _globalTimeout = (computed < AUTOCAL_GLOBAL_TIMEOUT_MS) ? computed : (unsigned long)AUTOCAL_GLOBAL_TIMEOUT_MS;
  }
  _timeoutEvent = false;
  memset(_results, 0, sizeof(_results));
  _audio.setActive(true);

  // Bring the air source (pump / reservoir / fan) up to a representative running
  // state before anything is measured. Passive modes report ready at once; active
  // ones converge as InstrumentManager::update() keeps ticking their controllers.
  _airSupply.prepare();
  _airReady = _airSupply.isReady();
  _airPrepareTime = _startTime;

  _rfMinAngle = -1;
  _rfMaxAngle = -1;
  _rfFoundMin = false;
  _rfLossCount = 0;
  _rfFailReason = ACAL_FAIL_NONE;
  _lastAirError = CAL_AIR_OK;
  _currentAngle = cfg.servoAirflowOff;
  _phase = PH_PREPARE;
  _stateTimer = _startTime;

  if (mode == ACAL_MODE_RANGE_FIND) {
    _state = ACAL_RANGE;
    if (DEBUG) Serial.println("DEBUG: AutoCalibrator - Demarrage range finder");
  } else {
    _state = ACAL_AIRFLOW;
    if (DEBUG) Serial.println("DEBUG: AutoCalibrator - Demarrage calibration airflow");
  }
}

void AutoCalibrator::safeHardware() {
  _airflow.testSolenoid(false);
  _airflow.setAirflowToRest();
  _fingers.closeAllFingers();
  _airSupply.stopSafe();
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
  if (now - _startTime >= _globalTimeout) {
    abortTimeout();
    return;
  }

  // Hold the whole run until the shared air source is ready. Nothing is measured
  // (and no note is scored) while the pump/reservoir/fan is still coming up.
  if (!_airReady) {
    CalAirSupplyError e = _airSupply.getError();
    if (e == CAL_AIR_SENSOR_FAULT || e == CAL_AIR_FAULT) {
      // Hard fault (e.g. reservoir mode with no usable sensor): do not pressurise
      // blind and do not wait out the readiness timeout - abort now.
      abortAirSupply();
      return;
    }
    if (_airSupply.isReady()) {
      _airReady = true;
    } else if (now - _airPrepareTime >= AUTOCAL_AIR_READY_TIMEOUT_MS) {
      abortAirSupply();
      return;
    } else {
      return; // keep waiting; controllers are ticked by InstrumentManager::update()
    }
  }

  updateStateMachine(now);
}

void AutoCalibrator::abortAirSupply() {
  // The air source never became usable: every note fails for the same reason and
  // nothing is written to the configuration. Mirrors the global-timeout path so the
  // UI reports a completed-but-failed calibration rather than a silent stall.
  _lastAirError = _airSupply.getError();
  for (int i = 0; i < _numNotes && i < MAX_NOTES; i++) {
    _results[i].valid = false;
    _results[i].failureReason = ACAL_FAIL_AIR_SUPPLY;
  }
  safeHardware();
  if (_mode == ACAL_MODE_RANGE_FIND) {
    _rfMinAngle = -1;
    _rfMaxAngle = -1;
    _rfFailReason = ACAL_FAIL_AIR_SUPPLY;
    _state = ACAL_RF_COMPLETE;
  } else {
    _state = ACAL_COMPLETE;
  }
  if (DEBUG) Serial.println("ERREUR: AutoCalibrator - Air non pret, calibration echouee");
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
    case PH_RF_SWEEP: return "range";
  }
  return "?";
}

float AutoCalibrator::currentToleranceCents() const {
  // Wide tolerance ONLY to detect the note's very first coarse appearance. The
  // coarse plateau (after onset), fine min, fine max and nominal candidates all
  // require the strict tolerance so a genuinely stable in-tune zone is proven.
  if (_phase == PH_COARSE && !_coarseValidSeen) return AUTOCAL_PITCH_TOLERANCE_ONSET_CENTS;
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

void AutoCalibrator::updateStateMachine(unsigned long now) {
  // Per-note timeout: fail this note (its previous calibration is kept) and let
  // the sweep continue with the next note. The global timeout stays last-resort.
  // The range finder is a single "note" (one angle sweep), so the same budget
  // bounds it too — routed to its own terminal state.
  if (_phase != PH_PREPARE && _phase != PH_FINALIZE &&
      (now - _noteStartTime) >= AUTOCAL_TIMEOUT_PER_NOTE_MS) {
    failCurrentNote(ACAL_FAIL_NOTE_TIMEOUT);
    return;
  }

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
    case PH_RF_SWEEP:
      if (runPositionStep(now)) onPositionEvaluated(now);
      break;
    case PH_FINALIZE:
      finalizeNote(now);
      break;
  }
}

void AutoCalibrator::prepareNote(unsigned long now) {
  // Range finder calibrates a single middle note across the servo angle range.
  if (_mode == ACAL_MODE_RANGE_FIND) _currentNote = cfg.numNotes / 2;
  _stepIsAngle = false;
  _expectedMidi = cfg.notes[_currentNote].midiNote;
  _noteStartTime = now;
  _noteFailReason = ACAL_FAIL_NONE;
  _audioStale = false;
  _anySoundSeen = false;
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

  // Re-assert the shared air source at its representative demand and catch a
  // drop-out (pump stall / fan fault) as a specific per-note reason rather than a
  // misleading "no sound". The noise floor that follows is therefore measured with
  // the pump/fan running (valve closed), i.e. in the representative acoustic state.
  _airSupply.setDemandPercent(100);
  if (airSupplyLost()) {
    failCurrentNote(ACAL_FAIL_AIR_SUPPLY);
  } else {
    _phase = PH_NOISE;
  }
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
      _noiseCollectStart = now;
      _noiseAccum = 0;
      _noiseCount = 0;
      _noiseHaveSeq = false;
    }
    return;
  }

  // Same freshness contract as a position: only average DISTINCT audio sequences
  // (never fold a frozen/repeated frame in twice), and fail with a stale-audio
  // reason if the source stops delivering new frames before enough are gathered.
  if (now - _noiseCollectStart >= AUTOCAL_AUDIO_FRAME_TIMEOUT_MS &&
      _noiseCount < AUTOCAL_REQUIRED_VALID_FRAMES) {
    failCurrentNote(ACAL_FAIL_AUDIO_STALE);
    return;
  }
  if (now - _lastFrameTime >= AUTOCAL_FRAME_SAMPLE_MS) {
    uint32_t seq = _audio.getFrameSequence();
    if (!_noiseHaveSeq || seq != _noiseLastSeq) {
      _lastFrameTime = now;
      _noiseLastSeq = seq;
      _noiseHaveSeq = true;
      _noiseAccum += _audio.getRMS();
      _noiseCount++;
    }
  }

  // Finish once the measurement window elapsed AND enough distinct frames landed.
  if (now - _stateTimer >= AUTOCAL_NOISE_MEASURE_MS &&
      _noiseCount >= AUTOCAL_REQUIRED_VALID_FRAMES) {
    _noiseRms = _noiseAccum / (float)_noiseCount;
    _soundThreshold = AutoCalMath::computeSoundThreshold(_noiseRms);
    // Open the air path, then start the appropriate sweep.
    _airflow.testSolenoid(true);
    if (_mode == ACAL_MODE_RANGE_FIND) {
      _phase = PH_RF_SWEEP;
      beginAnglePosition(AUTOCAL_RF_MIN_SAFE_ANGLE, now);
    } else {
      _phase = PH_COARSE;
      beginPosition(0, now);
    }

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
  _stepIsAngle = false;
  _stepPercent = percent;
  _step = ST_SET;
  _frameCount = 0;
  _stateTimer = now;
}

void AutoCalibrator::beginAnglePosition(int angle, unsigned long now) {
  if (angle < 0) angle = 0;
  if (angle > 180) angle = 180;
  _stepIsAngle = true;
  _stepAngle = angle;
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
      // SET_AIRFLOW: move the servo (percent for airflow, raw angle for range
      // finder), then wait for it to settle.
      if (_stepIsAngle) {
        _currentAngle = _stepAngle;
        _airflow.testAirflowAngle((uint16_t)_stepAngle);
      } else {
        setAirflowPercent(_stepPercent);
      }
      _stateTimer = now;
      _step = ST_SETTLE;
      return false;

    case ST_SETTLE:
      // A mid-position air-source drop-out (pump stall / fan fault) is its own
      // failure, not a misleading "no sound" / "wrong note".
      if (airSupplyLost()) { failCurrentNote(ACAL_FAIL_AIR_SUPPLY); return false; }
      // WAIT_AIRFLOW_SETTLE: ignore the transient frames during servo travel.
      if (now - _stateTimer >= AUTOCAL_AIR_SETTLE_MS) {
        _frameCount = 0;
        _haveCollectedSeq = false;   // start fresh: accept the first frame of this position
        _lastFrameTime = now;
        _collectStartTime = now;
        _step = ST_COLLECT;
      }
      return false;

    case ST_COLLECT:
      // Air source must stay up while we are collecting for this position.
      if (airSupplyLost()) { failCurrentNote(ACAL_FAIL_AIR_SUPPLY); return false; }
      // COLLECT_AUDIO: gather several spaced, DISTINCT frames (never count the
      // same analysed frame twice).
      if (now - _collectStartTime >= AUTOCAL_AUDIO_FRAME_TIMEOUT_MS &&
          _frameCount < AUTOCAL_AUDIO_FRAMES_PER_STEP) {
        // The audio source stopped delivering fresh frames: treat as a frozen
        // source and fail this position (range finder routed to its own end).
        _audioStale = true;
        failCurrentNote(ACAL_FAIL_AUDIO_STALE);
        return false;
      }
      if (now - _lastFrameTime >= AUTOCAL_FRAME_SAMPLE_MS) {
        uint32_t seq = _audio.getFrameSequence();
        if (!_haveCollectedSeq || seq != _lastCollectedSeq) {
          _lastFrameTime = now;
          _lastCollectedSeq = seq;
          _haveCollectedSeq = true;
          sampleFrame(_frames[_frameCount]);
          _frameCount++;
          if (_frameCount >= AUTOCAL_AUDIO_FRAMES_PER_STEP) _step = ST_EVAL;
        }
        // If the sequence has not advanced, keep waiting (do not store a copy).
      }
      return false;

    case ST_EVAL: {
      // EVALUATE_AUDIO: aggregate frames into a position decision + metrics.
      float tol = currentToleranceCents();
      _lastStats = AutoCalMath::evaluatePosition(_frames, _frameCount, _expectedMidi,
                                                 _soundThreshold, tol, MIC_YIN_CONFIDENCE_MIN);
      _lastMeanRms = _lastStats.meanRms;
      if (_lastStats.meanRms > _soundThreshold) _anySoundSeen = true;
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

    case PH_RF_SWEEP: {
      // Range finder: multi-frame, noise-thresholded, exact-note validation at
      // each angle; first valid = min, confirmed loss over several positions = max.
      if (!_rfFoundMin) {
        if (valid && !over) {
          _rfFoundMin = true;
          _rfMinAngle = _stepAngle - AUTOCAL_RF_MARGIN_DEG;
          if (_rfMinAngle < AUTOCAL_RF_MIN_SAFE_ANGLE) _rfMinAngle = AUTOCAL_RF_MIN_SAFE_ANGLE;
          _rfLossCount = 0;
        }
      } else {
        if (valid && !over) {
          _rfLossCount = 0;
        } else {
          _rfLossCount++;
          if (over || _rfLossCount >= AUTOCAL_LOSS_CONFIRM_STEPS) {
            int lastValid = _stepAngle - _rfLossCount * AUTOCAL_RF_STEP_DEG;
            _rfMaxAngle = lastValid + AUTOCAL_RF_MARGIN_DEG;
            if (_rfMaxAngle > AUTOCAL_RF_MAX_SAFE_ANGLE) _rfMaxAngle = AUTOCAL_RF_MAX_SAFE_ANGLE;
            safeHardware();
            _state = ACAL_RF_COMPLETE;
            return;
          }
        }
      }
      int next = _stepAngle + AUTOCAL_RF_STEP_DEG;
      if (next > AUTOCAL_RF_MAX_SAFE_ANGLE) {
        if (_rfFoundMin && _rfMaxAngle < 0) _rfMaxAngle = AUTOCAL_RF_MAX_SAFE_ANGLE;
        safeHardware();
        _state = ACAL_RF_COMPLETE;
        return;
      }
      beginAnglePosition(next, now);
      break;
    }

    default:
      break;
  }
}

void AutoCalibrator::finalizeNote(unsigned long now) {
  AutoCalNoteResult& r = _results[_currentNote];
  memset(&r, 0, sizeof(r));

  // Diagnostics captured for both success and failure.
  r.lastDetectedMidi = (int16_t)_dispMidi;
  r.lastRms = _lastMeanRms;
  r.validFrames = (uint8_t)((_lastValidFrames < 0) ? 0 : (_lastValidFrames > 255 ? 255 : _lastValidFrames));
  {
    unsigned long dur = now - _noteStartTime;
    r.durationMs = (dur > 60000UL) ? 60000 : (uint16_t)dur;
  }

  // A note is valid ONLY when the fine sweeps found a range, a nominal candidate
  // was validated with the strict tolerance (_bestValidFound), and the final
  // confidence clears the acceptance threshold.
  bool rangeOk = _coarseValidSeen && _airMin >= 0 && _airMax >= 0 && _airMax >= _airMin;
  bool nominalOk = _bestValidFound && _bestPercent >= 0;
  uint8_t conf = _bestConfPct;
  bool confOk = conf >= AUTOCAL_MIN_RESULT_CONFIDENCE;

  if (rangeOk && nominalOk && confOk) {
    uint8_t mn = (uint8_t)((_airMin < 0) ? 0 : (_airMin > 100 ? 100 : _airMin));
    uint8_t mx = (uint8_t)((_airMax < 0) ? 0 : (_airMax > 100 ? 100 : _airMax));
    if (mx < mn) mx = mn;
    uint8_t nominal = AutoCalMath::clampNominal(mn, mx, (uint8_t)_bestPercent, AUTOCAL_NOMINAL_GUARD_PCT);

    r.valid = true;
    r.airMin = mn;
    r.airMax = mx;
    r.airNominal = nominal;
    r.medianCents = _bestCents;
    r.pitchStability = _bestStability;
    r.signalToNoiseRatio = _bestSnr;
    r.confidence = conf;
    r.failureReason = ACAL_FAIL_NONE;
  } else {
    r.valid = false;
    r.confidence = conf;
    // Most specific failure reason: a timeout/stale reason set during the sweep
    // wins; otherwise classify by how far the note got.
    uint8_t reason = _noteFailReason;
    if (reason == ACAL_FAIL_NONE) {
      if (!_coarseValidSeen) reason = _anySoundSeen ? ACAL_FAIL_WRONG_NOTE : ACAL_FAIL_NO_SOUND;
      else if (!nominalOk) reason = ACAL_FAIL_NO_STABLE_NOMINAL;
      else reason = ACAL_FAIL_LOW_CONFIDENCE;
    }
    r.failureReason = reason;
  }

  if (DEBUG) {
    Serial.print("DEBUG: AutoCal - Note ");
    Serial.print(_currentNote);
    Serial.print(r.valid ? " OK min=" : " FAIL(reason)=");
    Serial.print(r.valid ? r.airMin : r.failureReason);
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

bool AutoCalibrator::airSupplyLost() {
  if (_airSupply.isReady() && _airSupply.getError() == CAL_AIR_OK) return false;
  _lastAirError = _airSupply.getError();
  return true;
}

void AutoCalibrator::failCurrentNote(uint8_t reason) {
  if (reason == ACAL_FAIL_AIR_SUPPLY) _lastAirError = _airSupply.getError();
  // The range finder is not a per-note calibration: it must never run
  // finalizeNote()/advanceNote() (which would re-centre and loop). Route it to a
  // dedicated terminal state instead.
  if (_mode == ACAL_MODE_RANGE_FIND) {
    finalizeRangeFinderFailure(reason);
  } else {
    _noteFailReason = reason;
    _phase = PH_FINALIZE;
  }
}

void AutoCalibrator::finalizeRangeFinderFailure(uint8_t reason) {
  safeHardware();
  _rfMinAngle = -1;   // invalid: apply must refuse it
  _rfMaxAngle = -1;
  _rfFailReason = reason;
  _state = ACAL_RF_COMPLETE;
  if (DEBUG) {
    Serial.print("ERREUR: RangeFinder - echec, reason=");
    Serial.println(failureReasonName(reason));
  }
}

const char* AutoCalibrator::failureReasonName(uint8_t reason) {
  switch (reason) {
    case ACAL_FAIL_NONE:              return "none";
    case ACAL_FAIL_NO_SOUND:          return "no_sound";
    case ACAL_FAIL_WRONG_NOTE:        return "wrong_note";
    case ACAL_FAIL_LOW_CONFIDENCE:    return "low_confidence";
    case ACAL_FAIL_LOW_SNR:           return "low_snr";
    case ACAL_FAIL_UNSTABLE_PITCH:    return "unstable_pitch";
    case ACAL_FAIL_NO_STABLE_NOMINAL: return "no_stable_nominal";
    case ACAL_FAIL_AUDIO_STALE:       return "audio_stale";
    case ACAL_FAIL_NOTE_TIMEOUT:      return "note_timeout";
    case ACAL_FAIL_GLOBAL_TIMEOUT:    return "global_timeout";
    case ACAL_FAIL_AIR_SUPPLY:        return "air_supply";
    case ACAL_FAIL_STORAGE:           return "storage";
  }
  return "unknown";
}

const char* AutoCalibrator::airSupplyErrorName(CalAirSupplyError err) {
  switch (err) {
    case CAL_AIR_OK:           return "ok";
    case CAL_AIR_NOT_READY:    return "not_ready";
    case CAL_AIR_TIMEOUT:      return "timeout";
    case CAL_AIR_SENSOR_FAULT: return "sensor_fault";
    case CAL_AIR_FAULT:        return "fault";
  }
  return "unknown";
}

// ----------------------------------------------------------------- persistence -

AutoCalApplyResult AutoCalibrator::applyResults() {
  AutoCalApplyResult out{false, false, 0, 0};

  int n = (_numNotes < (int)cfg.numNotes) ? _numNotes : (int)cfg.numNotes;
  for (int i = 0; i < n; i++) {
    if (_results[i].valid) out.validCount++;
    else out.failedCount++;
  }

  // Nothing valid: never claim an apply or a save.
  if (out.validCount == 0) {
    if (DEBUG) Serial.println("DEBUG: AutoCalibrator - Aucun resultat valide, rien a sauvegarder");
    return out;
  }

  // Back up the three fields we may change so we can restore RAM on a save error.
  uint8_t bMin[MAX_NOTES], bMax[MAX_NOTES], bNom[MAX_NOTES];
  for (int i = 0; i < n; i++) {
    bMin[i] = cfg.notes[i].airflowMinPercent;
    bMax[i] = cfg.notes[i].airflowMaxPercent;
    bNom[i] = cfg.notes[i].airflowNominalPercent;
  }

  // Never overwrite a previously valid calibration with a failed new one.
  for (int i = 0; i < n; i++) {
    if (_results[i].valid) {
      cfg.notes[i].airflowMinPercent = _results[i].airMin;
      cfg.notes[i].airflowMaxPercent = _results[i].airMax;
      cfg.notes[i].airflowNominalPercent = _results[i].airNominal;
    }
  }
  out.saved = ConfigStorage::save();

  if (!out.saved) {
    // Storage failed: roll the configuration back in RAM. The values are NOT in
    // effect, so applied must be false too - the caller must not report success.
    for (int i = 0; i < n; i++) {
      cfg.notes[i].airflowMinPercent = bMin[i];
      cfg.notes[i].airflowMaxPercent = bMax[i];
      cfg.notes[i].airflowNominalPercent = bNom[i];
    }
    out.applied = false;
    if (DEBUG) Serial.println("ERREUR: AutoCalibrator - Sauvegarde echouee, config restauree en RAM");
  } else {
    out.applied = true;
    if (DEBUG) Serial.println("DEBUG: AutoCalibrator - Resultats appliques et sauvegardes");
  }
  return out;
}

RangeApplyResult AutoCalibrator::applyRangeResults() {
  RangeApplyResult out{false, false, -1, -1};

  // Refuse an invalid / failed range-finder result: change nothing.
  if (_rfMinAngle < 0 || _rfMaxAngle < 0 || _rfMaxAngle < _rfMinAngle) {
    if (DEBUG) Serial.println("DEBUG: RangeFinder - resultat invalide, rien a appliquer");
    return out;
  }

  const uint16_t bMin = cfg.servoAirflowMin;
  const uint16_t bMax = cfg.servoAirflowMax;
  cfg.servoAirflowMin = _rfMinAngle;
  cfg.servoAirflowMax = _rfMaxAngle;

  out.saved = ConfigStorage::save();
  if (!out.saved) {
    // Storage failed: restore the previous angles; nothing is in effect.
    cfg.servoAirflowMin = bMin;
    cfg.servoAirflowMax = bMax;
    if (DEBUG) Serial.println("ERREUR: RangeFinder - Sauvegarde echouee, angles restaures en RAM");
    return out;
  }

  out.applied = true;
  out.minAngle = cfg.servoAirflowMin;
  out.maxAngle = cfg.servoAirflowMax;
  if (DEBUG) {
    Serial.print("DEBUG: RangeFinder - Applique min=");
    Serial.print(cfg.servoAirflowMin);
    Serial.print(" max=");
    Serial.println(cfg.servoAirflowMax);
  }
  return out;
}

#endif // MIC_ENABLED

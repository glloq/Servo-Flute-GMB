/***********************************************************************************************
 * AutoCalibrator - Automatic per-note airflow calibration using an INMP441 microphone
 *
 * Airflow mode: for each note the calibrator
 *   1. positions the fingers and measures the background noise floor (valve closed);
 *   2. derives an adaptive RMS sound threshold from that noise;
 *   3. runs a COARSE airflow sweep to bracket the note's playable region;
 *   4. runs FINE sweeps to pin down airflowMin and airflowMax precisely;
 *   5. scores candidate points to pick a recommended nominal airflow;
 *   6. produces min / nominal / max, a 0-100 confidence, cents error, pitch
 *      stability and SNR.
 *
 * Every airflow position is validated over several audio frames (SET -> SETTLE ->
 * COLLECT -> EVALUATE) so a single noisy frame cannot create a false positive.
 * A note is accepted only when the detected MIDI note exactly matches the expected
 * one within a cents tolerance; neighbour notes and the octave above (overblow) are
 * rejected. An octave above appearing after a valid plateau marks the upper limit.
 *
 * The state machine is fully non-blocking (no delay()) and is bounded by a global
 * firmware timeout. On stop / timeout / error the hardware is returned to a safe
 * state.
 *
 * Range-finder mode sweeps the airflow servo 0-180 deg on a middle note to discover
 * the usable servo travel.
 *
 * The calibrator depends on IAudioSource (not the concrete I2S driver) so it can be
 * unit-tested with simulated audio.
 ***********************************************************************************************/
#ifndef AUTO_CALIBRATOR_H
#define AUTO_CALIBRATOR_H

#include <Arduino.h>
#include "settings.h"

#if MIC_ENABLED

#include "AutoCalMath.h"

class FingerController;
class AirflowController;
class IAudioSource;
class ICalibrationAirSupply;

enum AutoCalMode {
  ACAL_MODE_AIRFLOW,        // Auto-calibrate airflow per note
  ACAL_MODE_RANGE_FIND      // Find global servo range (min/max)
};

// Top-level lifecycle state (kept coarse; the detailed sequence lives in _phase).
enum AutoCalState {
  ACAL_IDLE,
  ACAL_AIRFLOW,       // per-note airflow calibration running
  ACAL_COMPLETE,      // per-note airflow calibration finished
  ACAL_RANGE,         // range finder running
  ACAL_RF_COMPLETE    // range finder finished
};

// Reason a note failed calibration (ACAL_FAIL_NONE when it succeeded).
enum AutoCalFailureReason {
  ACAL_FAIL_NONE = 0,
  ACAL_FAIL_NO_SOUND,          // note never appeared above the noise floor
  ACAL_FAIL_WRONG_NOTE,        // sound present but never the expected MIDI note
  ACAL_FAIL_LOW_CONFIDENCE,    // final confidence below AUTOCAL_MIN_RESULT_CONFIDENCE
  ACAL_FAIL_LOW_SNR,           // reserved: signal too close to noise
  ACAL_FAIL_UNSTABLE_PITCH,    // reserved: pitch too unstable
  ACAL_FAIL_NO_STABLE_NOMINAL, // no candidate validated with the strict tolerance
  ACAL_FAIL_AUDIO_STALE,       // audio source stopped producing fresh frames
  ACAL_FAIL_NOTE_TIMEOUT,      // per-note timeout elapsed
  ACAL_FAIL_GLOBAL_TIMEOUT,    // global timeout elapsed
  ACAL_FAIL_AIR_SUPPLY,        // air supply not ready / lost
  ACAL_FAIL_STORAGE            // reserved: persistence failure
};

// Per-note calibration result (also broadcast to the web UI and persisted).
struct AutoCalNoteResult {
  bool valid;
  uint8_t airMin;            // 0-100 %
  uint8_t airMax;            // 0-100 %
  uint8_t airNominal;        // 0-100 %, always airMin <= airNominal <= airMax
  uint8_t confidence;        // 0-100
  float medianCents;         // cents error at the nominal point
  float pitchStability;      // 0..1 (1 = perfectly stable)
  float signalToNoiseRatio;  // dB
  // Diagnostics (populated for both success and failure).
  uint8_t failureReason;     // AutoCalFailureReason
  int16_t lastDetectedMidi;  // last MIDI note the analyzer reported
  float lastRms;             // last position mean RMS
  uint8_t validFrames;       // valid frames of the best/last evaluated position
  uint16_t durationMs;       // time spent calibrating this note (capped)
};

// Aggregate outcome of applying results (persistence-aware).
struct AutoCalApplyResult {
  bool applied;        // at least one valid note written into cfg
  bool saved;          // persisted to LittleFS successfully
  uint8_t validCount;
  uint8_t failedCount;
};

class AutoCalibrator {
public:
  AutoCalibrator(FingerController& fingers, AirflowController& airflow, IAudioSource& audio,
                 ICalibrationAirSupply& airSupply);

  void start(AutoCalMode mode);
  void stop();
  void update();

  bool isRunning() const { return _state == ACAL_AIRFLOW || _state == ACAL_RANGE; }
  bool isComplete() const { return _state == ACAL_COMPLETE; }
  bool isRangeFinderComplete() const { return _state == ACAL_RF_COMPLETE; }
  bool isRangeMode() const { return _mode == ACAL_MODE_RANGE_FIND; }
  AutoCalState getState() const { return _state; }

  // One-shot timeout event: returns true once after a global-timeout abort so the
  // caller can publish an error. Cleared by reading.
  bool takeTimeoutEvent() { bool t = _timeoutEvent; _timeoutEvent = false; return t; }

  // --- Live progress (WebSocket) ---
  int getCurrentNoteIndex() const { return _currentNote; }
  int getTotalNotes() const { return _numNotes; }
  const char* getPhaseName() const;
  int getCurrentAirPercent() const { return _stepPercent; }
  int getCurrentAngle() const { return _currentAngle; }   // range-finder sweep angle
  float getNoiseRms() const { return _noiseRms; }
  float getLastRms() const { return _lastMeanRms; }
  float getLastHz() const { return _dispHz; }
  int getLastMidi() const { return _dispMidi; }
  float getLastCents() const { return _dispCents; }
  uint8_t getLastConfidence() const { return _lastConfidencePct; }
  int getLastValidFrames() const { return _lastValidFrames; }
  int getLastTotalFrames() const { return _lastTotalFrames; }

  // Results after ACAL_COMPLETE
  const AutoCalNoteResult* getResults() const { return _results; }
  AutoCalNoteResult getResult(int idx) const { return _results[idx]; }
  // Writes only valid results into cfg and persists; reports what happened and,
  // on a storage failure, restores the previous configuration in RAM.
  AutoCalApplyResult applyResults();

  // Range finder results
  int getRangeFinderMin() const { return _rfMinAngle; }
  int getRangeFinderMax() const { return _rfMaxAngle; }
  void applyRangeResults();

private:
  // Detailed per-note phase and per-position sub-step. The range finder reuses
  // the same per-position engine (PH_RF_SWEEP), so there is no duplicated logic.
  enum Phase { PH_PREPARE, PH_NOISE, PH_COARSE, PH_FINE_MIN, PH_FINE_MAX, PH_NOMINAL, PH_FINALIZE, PH_RF_SWEEP };
  enum Step  { ST_SET, ST_SETTLE, ST_COLLECT, ST_EVAL };

  FingerController& _fingers;
  AirflowController& _airflow;
  IAudioSource& _audio;
  ICalibrationAirSupply& _airSupply;

  // Air-source readiness gate (pump spin-up / reservoir fill / fan ramp). Passive
  // modes report ready immediately. Failure aborts the whole run (shared source).
  bool _airReady;
  unsigned long _airPrepareTime;

  AutoCalState _state;
  AutoCalMode _mode;
  unsigned long _startTime;    // for the global timeout
  unsigned long _globalTimeout;  // computed = numNotes*perNote + margin (capped)
  bool _timeoutEvent;

  int _numNotes;               // snapshot of cfg.numNotes at start
  int _currentNote;
  uint8_t _expectedMidi;

  Phase _phase;
  Step _step;
  unsigned long _stateTimer;
  unsigned long _lastFrameTime;
  int _stepPercent;            // airflow percent currently being evaluated (airflow mode)
  bool _stepIsAngle;           // true: the position is a raw servo angle (range finder)
  int _stepAngle;              // raw servo angle currently being evaluated (range finder)

  // Per-position frame collection (only distinct, fresh frames are stored)
  AutoCalMath::AudioFrame _frames[AUTOCAL_AUDIO_FRAMES_PER_STEP];
  int _frameCount;
  uint32_t _lastCollectedSeq;   // sequence of the last stored frame (0 = none yet)
  bool _haveCollectedSeq;       // whether _lastCollectedSeq is meaningful
  unsigned long _collectStartTime;  // for the per-position audio-stale timeout
  bool _audioStale;             // last collection ended without enough fresh frames

  // Per-note timing / failure tracking
  unsigned long _noteStartTime;
  uint8_t _noteFailReason;      // AutoCalFailureReason accumulated for this note
  bool _anySoundSeen;           // any position had sound above the noise threshold

  // Latest evaluated position (used by phase logic + progress + scoring)
  AutoCalMath::PositionStats _lastStats;
  float _lastMeanRms;
  int _lastValidFrames;
  int _lastTotalFrames;
  float _lastStability;
  float _lastSnr;
  float _lastMeanConf;
  float _lastValidRatio;
  uint8_t _lastConfidencePct;
  // Representative pitch for display (valid median, else last frame)
  float _dispHz;
  int _dispMidi;
  float _dispCents;

  // Noise floor
  float _noiseRms;
  float _soundThreshold;
  float _noiseAccum;
  int _noiseCount;
  bool _noiseSettled;

  // Coarse pass tracking
  int _coarseMin;              // percent, -1 if not found
  int _coarseMax;
  bool _coarseValidSeen;
  int _coarseLossCount;

  // Fine pass results
  int _airMin;                 // percent, -1 if not found
  int _airMax;
  int _fineLastValid;
  int _fineLossCount;

  // Nominal candidate scan
  uint8_t _nominalCandidates[5];
  int _nominalCount;
  int _nominalIndex;

  // Running best candidate for the nominal point
  bool _bestValidFound;
  int _bestPercent;
  float _bestScore;
  float _bestCents;
  float _bestStability;
  float _bestSnr;
  uint8_t _bestConfPct;

  // Range finder (shares the per-position engine via PH_RF_SWEEP)
  int _currentAngle;
  int _rfMinAngle;
  int _rfMaxAngle;
  bool _rfFoundMin;
  int _rfLossCount;

  AutoCalNoteResult _results[MAX_NOTES];

  // --- helpers ---
  void updateStateMachine(unsigned long now);
  void prepareNote(unsigned long now);
  void runNoise(unsigned long now);
  bool runPositionStep(unsigned long now);   // true when a fresh evaluation is ready
  void onPositionEvaluated(unsigned long now);
  void beginPosition(int percent, unsigned long now);
  void beginAnglePosition(int angle, unsigned long now);
  void sampleFrame(AutoCalMath::AudioFrame& f);
  void finalizeNote(unsigned long now);
  void advanceNote(unsigned long now);
  float currentToleranceCents() const;
  void setAirflowPercent(int percent);
  void safeHardware();
  void abortTimeout();
  void abortAirSupply();  // air source never reached a usable state: fail all notes
};

#endif // MIC_ENABLED
#endif // AUTO_CALIBRATOR_H

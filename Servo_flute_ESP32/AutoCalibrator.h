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
};

class AutoCalibrator {
public:
  AutoCalibrator(FingerController& fingers, AirflowController& airflow, IAudioSource& audio);

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
  void applyResults();   // writes valid results into cfg and persists

  // Range finder results
  int getRangeFinderMin() const { return _rfMinAngle; }
  int getRangeFinderMax() const { return _rfMaxAngle; }
  void applyRangeResults();

private:
  // Detailed per-note phase and per-position sub-step.
  enum Phase { PH_PREPARE, PH_NOISE, PH_COARSE, PH_FINE_MIN, PH_FINE_MAX, PH_NOMINAL, PH_FINALIZE };
  enum Step  { ST_SET, ST_SETTLE, ST_COLLECT, ST_EVAL };
  enum RfStep { RF_PREPARE, RF_SETTLE, RF_SWEEP };

  FingerController& _fingers;
  AirflowController& _airflow;
  IAudioSource& _audio;

  AutoCalState _state;
  AutoCalMode _mode;
  unsigned long _startTime;    // for the global timeout
  bool _timeoutEvent;

  int _numNotes;               // snapshot of cfg.numNotes at start
  int _currentNote;
  uint8_t _expectedMidi;

  Phase _phase;
  Step _step;
  unsigned long _stateTimer;
  unsigned long _lastFrameTime;
  int _stepPercent;            // airflow percent currently being evaluated

  // Per-position frame collection
  AutoCalMath::AudioFrame _frames[AUTOCAL_AUDIO_FRAMES_PER_STEP];
  int _frameCount;

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

  // Range finder
  RfStep _rfStep;
  int _currentAngle;
  int _rfMinAngle;
  int _rfMaxAngle;
  bool _rfFoundMin;
  int _rfSilenceCount;

  AutoCalNoteResult _results[MAX_NOTES];

  // --- helpers ---
  void updateAirflow(unsigned long now);
  void updateRangeFinder(unsigned long now);
  void prepareNote(unsigned long now);
  void runNoise(unsigned long now);
  bool runPositionStep(unsigned long now);   // true when a fresh evaluation is ready
  void onPositionEvaluated(unsigned long now);
  void beginPosition(int percent, unsigned long now);
  void sampleFrame(AutoCalMath::AudioFrame& f);
  void finalizeNote(unsigned long now);
  void advanceNote(unsigned long now);
  float currentToleranceCents() const;
  void setAirflowPercent(int percent);
  void safeHardware();
  void abortTimeout();
};

#endif // MIC_ENABLED
#endif // AUTO_CALIBRATOR_H

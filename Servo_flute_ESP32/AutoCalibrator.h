/***********************************************************************************************
 * AutoCalibrator - Automatic calibration using INMP441 microphone
 *
 * Airflow mode: For each note, sweeps airflow from off to max while listening
 * for sound onset/offset to determine optimal air_min and air_max percentages.
 *
 * Uses FingerController + AirflowController for servo control
 * and AudioAnalyzer for sound detection + pitch verification.
 *
 * Call update() from main loop. Progress reported via getters.
 ***********************************************************************************************/
#ifndef AUTO_CALIBRATOR_H
#define AUTO_CALIBRATOR_H

#include <Arduino.h>
#include "settings.h"

#if MIC_ENABLED

class FingerController;
class AirflowController;
class AudioAnalyzer;

enum AutoCalMode {
  ACAL_MODE_AIRFLOW     // Auto-calibrate airflow per note
};

enum AutoCalState {
  ACAL_IDLE,
  ACAL_PREPARE,         // Position fingers, open solenoid
  ACAL_SETTLE,          // Wait for servos to settle
  ACAL_SWEEP,           // Sweeping airflow angle
  ACAL_NOTE_DONE,       // Store result, prepare next
  ACAL_COMPLETE         // All notes calibrated
};

struct AutoCalNoteResult {
  bool valid;
  uint8_t airMin;       // Percentage (0-100)
  uint8_t airMax;       // Percentage (0-100)
};

class AutoCalibrator {
public:
  AutoCalibrator(FingerController& fingers, AirflowController& airflow, AudioAnalyzer& audio);

  void start(AutoCalMode mode);
  void stop();
  void update();

  bool isRunning() const { return _state != ACAL_IDLE && _state != ACAL_COMPLETE; }
  bool isComplete() const { return _state == ACAL_COMPLETE; }
  AutoCalState getState() const { return _state; }
  int getCurrentNoteIndex() const { return _currentNote; }
  int getCurrentAngle() const { return _currentAngle; }
  bool hasFoundMin() const { return _foundMin; }
  int getAirMinPct() const { return _airMinPct; }
  int getAirMaxPct() const { return _airMaxPct; }

  // Results after ACAL_COMPLETE
  const AutoCalNoteResult* getResults() const { return _results; }
  AutoCalNoteResult getResult(int idx) const { return _results[idx]; }

  // Apply results to cfg
  void applyResults();

private:
  FingerController& _fingers;
  AirflowController& _airflow;
  AudioAnalyzer& _audio;

  AutoCalState _state;
  AutoCalMode _mode;
  int _currentNote;         // Index in cfg.notes[]
  int _currentAngle;        // Current servo angle
  unsigned long _stateTimer;

  bool _foundMin;
  int _airMinPct;
  int _airMaxPct;
  int _silenceCounter;      // Consecutive silent readings after sound found

  AutoCalNoteResult _results[MAX_NOTES];

  int angleToPct(int angle);
  void advanceToNextNote();
};

#endif // MIC_ENABLED
#endif // AUTO_CALIBRATOR_H

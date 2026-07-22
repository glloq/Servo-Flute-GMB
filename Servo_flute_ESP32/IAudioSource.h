/***********************************************************************************************
 * IAudioSource - Abstract audio measurement source
 *
 * Decouples the AutoCalibrator from the concrete I2S microphone driver
 * (AudioAnalyzer). The calibrator only needs the analysed measurements of the
 * latest frame, never the raw I2S buffers, so it depends on this interface.
 *
 * On the firmware target AudioAnalyzer implements it. In the native unit tests a
 * lightweight fake implements it to inject simulated measurements without any
 * real hardware.
 ***********************************************************************************************/
#ifndef IAUDIO_SOURCE_H
#define IAUDIO_SOURCE_H

class IAudioSource {
public:
  virtual ~IAudioSource() {}

  // Lifecycle / detection
  virtual bool isMicDetected() const = 0;
  virtual void setActive(bool active) = 0;
  virtual bool isActive() const = 0;

  // Latest analysed frame
  virtual float getRMS() const = 0;          // RMS of the DC-removed frame
  virtual bool  isSoundDetected() const = 0; // RMS above the monitoring gate
  virtual float getPitchHz() const = 0;      // fundamental (Hz), 0 if none
  virtual int   getPitchMidi() const = 0;    // nearest MIDI note, 0 if none
  virtual float getPitchCents() const = 0;   // signed cents deviation
  virtual float getPitchConfidence() const = 0; // YIN confidence 0..1
  virtual bool  isPitchValid() const = 0;    // confidence high enough to trust the pitch
};

#endif  // IAUDIO_SOURCE_H

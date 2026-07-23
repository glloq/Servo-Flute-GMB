#include <cassert>
#include <iostream>
#include <map>
#include "Arduino.h"
#include "Wire.h"
#include "ConfigStorage.h"
#define private public
#include "PressureController.h"
#undef private
#include "FanController.h"
#include "EventQueue.h"
#include "FingerController.h"
#include "AirflowController.h"
#include "NoteSequencer.h"
#include "InstrumentManager.h"
#include "IAudioSource.h"
#include "ICalibrationAirSupply.h"
#include "CalibrationAirSupply.h"
#include "PitchMath.h"
#include "AutoCalMath.h"
#include "AutoCalibrator.h"
#include "PitchDetector.h"
#include <cmath>
#include <vector>
extern std::map<uint8_t,int> __analog_writes, __digital_writes, __analog_reads, __digital_reads;

// --- Simulated audio source for AutoCalibrator tests (no real I2S hardware) ---
struct FakeAudio : public IAudioSource {
  bool micDetected = true, active = false;
  float rms = 0, hz = 0, cents = 0, conf = 0; int midi = 0; bool valid = false, snd = false;
  uint32_t seq = 0; unsigned long ts = 0;
  bool isMicDetected() const override { return micDetected; }
  void setActive(bool a) override { active = a; }
  bool isActive() const override { return active; }
  float getRMS() const override { return rms; }
  bool isSoundDetected() const override { return snd; }
  float getPitchHz() const override { return hz; }
  int getPitchMidi() const override { return midi; }
  float getPitchCents() const override { return cents; }
  float getPitchConfidence() const override { return conf; }
  bool isPitchValid() const override { return valid; }
  uint32_t getFrameSequence() const override { return seq; }
  unsigned long getFrameTimestamp() const override { return ts; }
};
// --- Simulated air supply (pump/fan/reservoir) for AutoCalibrator tests ---
// `readyAfter` frames of prepare() before isReady() flips true; UINT32_MAX = never.
struct FakeAirSupply : public ICalibrationAirSupply {
  bool prepared = false; uint32_t readyPolls = 0, readyAfter = 0;
  uint8_t lastDemand = 0; bool stopped = false; CalAirSupplyError err = CAL_AIR_OK;
  // Mid-run drop-out: once forceNotReady is set the source reports not-ready and
  // surfaces dropError (used to simulate a pump stall / fan fault during a note).
  bool forceNotReady = false; CalAirSupplyError dropError = CAL_AIR_FAULT;
  void prepare() override { prepared = true; stopped = false; readyPolls = 0; }
  bool isReady() override {
    if (forceNotReady) { err = dropError; return false; }
    return prepared && (readyPolls++ >= readyAfter);
  }
  void setDemandPercent(uint8_t p) override { lastDemand = p; }
  void stopSafe() override { stopped = true; }
  CalAirSupplyError getError() const override { return err; }
};
static AutoCalMath::AudioFrame mkFrame(bool valid, float rms, int midi, float cents, float conf) {
  AutoCalMath::AudioFrame f;
  f.pitchValid = valid; f.rms = rms; f.hz = (midi > 0) ? PitchMath::midiToHz(midi) : 0.0f;
  f.midi = midi; f.cents = cents; f.confidence = conf; return f;
}
// Drive a running calibration to completion, feeding simulated audio each cycle.
// When freshFrames is true a new frame sequence is produced each cycle (a live
// microphone); when false the sequence is frozen (a stuck/disconnected source).
template <class Model>
static void driveAutoCal(AutoCalibrator& cal, FakeAudio& fa, Model model,
                         bool freshFrames = true, int maxIter = 200000) {
  int it = 0;
  while (cal.isRunning() && it++ < maxIter) {
    model(cal.getCurrentAirPercent(), cal.getCurrentNoteIndex(), fa);
    if (freshFrames) { fa.seq++; fa.ts = __test_millis; }
    __test_millis += 25;
    cal.update();
  }
}
// Realistic per-note audio model: silence below LO, the note in [LO,HI], overblow above.
static void bandModel(int pct, int expectedMidi, int lo, int hi, FakeAudio& f) {
  if (pct >= lo && pct <= hi) {
    f.rms = 0.06f; f.valid = true; f.conf = 0.95f; f.midi = expectedMidi;
    f.hz = PitchMath::midiToHz(expectedMidi); f.cents = -4.0f; f.snd = true;
  } else if (pct > hi) {
    f.rms = 0.06f; f.valid = true; f.conf = 0.95f; f.midi = expectedMidi + 12;
    f.hz = PitchMath::midiToHz(expectedMidi + 12); f.cents = -4.0f; f.snd = true;
  } else {
    f.rms = 0.003f; f.valid = false; f.conf = 0.0f; f.midi = 0; f.hz = 0; f.cents = 0; f.snd = false;
  }
}
static void resetCfg(){ memset(&cfg,0,sizeof(cfg)); cfg.numFingers=1; cfg.fingers[0].pcaChannel=0; cfg.fingers[0].closedAngle=90; cfg.fingers[0].direction=1; cfg.numPumps=1; cfg.pumpPins[0]=25; cfg.pumpPins[1]=26; cfg.pumpPins[2]=27; cfg.pumpMinPwm[0]=80; cfg.pumpMaxPwm[0]=200; cfg.pumpMinPwm[1]=90; cfg.pumpMaxPwm[1]=220; cfg.pumpMinPwm[2]=110; cfg.pumpMaxPwm[2]=255; cfg.motorType=MOTOR_TYPE_PWM; cfg.airMode=AIR_MODE_PUMP_VALVE; cfg.pumpCascadeThreshold=0; cfg.pumpStaggerMs=0; cfg.sensorType=SENSOR_TYPE_HALL_KY024; cfg.hallPin=36; cfg.hallThresholdLow=1000; cfg.hallThresholdHigh=2000; cfg.pidKp=10; cfg.pidKi=0; cfg.servoToSolenoidDelayMs=10; cfg.minNoteDurationMs=100; cfg.minNoteIntervalForValveCloseMs=0; cfg.airflowPcaChannel=10; cfg.solenoidPin=13; cfg.solenoidActivationTimeMs=50; cfg.solenoidPwmActivation=255; cfg.solenoidPwmHolding=128; cfg.servoAirflowOff=20; cfg.servoAirflowMin=60; cfg.servoAirflowMax=100; cfg.servoAngleOff=90; cfg.servoAngleMin=45; cfg.servoAngleMax=135; cfg.ccVolumeDefault=127; cfg.ccExpressionDefault=127; cfg.ccBreathDefault=127; cfg.ccBrightnessDefault=64; cfg.airVelocityResponse=100; strcpy(cfg.embouchure,"bec"); cfg.numNotes=3; cfg.notes[0].midiNote=60; cfg.notes[1].midiNote=61; cfg.notes[2].midiNote=62; cfg.notes[0].airflowMinPercent=0; cfg.notes[0].airflowMaxPercent=100; }
static int physical(int minv,int maxv,int logical){ return logical==0?0:minv+(maxv-minv)*logical/255; }
static void pressure_direct_pwm_once(){ resetCfg(); PressureController pc; for(int logical: {0,64,128,192,255}){ pc.setPumpPwm(logical); assert(__analog_writes[25]==physical(80,200,logical)); } cfg.numPumps=3; cfg.pumpCascadeThreshold=0; pc.setPumpPwm(128); assert(__analog_writes[25]==physical(80,200,128)); assert(__analog_writes[26]==physical(90,220,128)); assert(__analog_writes[27]==physical(110,255,128)); cfg.motorType=MOTOR_TYPE_ONOFF; pc.setPumpPwm(128); assert(__digital_writes[25]==HIGH); assert(__digital_writes[26]==HIGH); assert(__digital_writes[27]==HIGH); pc.setPumpPwm(0); assert(__digital_writes[25]==LOW); }
static void pressure_hall_pid_once_and_guards(){ resetCfg(); cfg.airMode=AIR_MODE_PUMP_RESERVOIR; PressureController pc; pc.begin(); pc.setTargetPercent(50); __test_millis=100; __analog_reads[36]=1000; pc.update(); assert(__analog_writes[25]==physical(80,200,127)); cfg.hallThresholdHigh=cfg.hallThresholdLow; __test_millis=200; pc.update(); assert(__analog_writes[25]>=0 && __analog_writes[25]<=255); cfg.numPumps=0; __test_millis=300; pc.update(); cfg.numPumps=9; cfg.pumpCascadeThreshold=100; validateAndNormalizeConfig(cfg,nullptr); assert(cfg.numPumps==MAX_PUMPS && cfg.pumpCascadeThreshold==99); }
static void event_queue_cases(){ EventQueue q(2); assert(q.isEmpty()); assert(q.enqueueScheduledEvent(EVENT_NOTE_ON,60,100,0)); assert(q.enqueueScheduledEvent(EVENT_NOTE_OFF,60,0,20)); assert(!q.enqueueScheduledEvent(EVENT_NOTE_ON,61,100,20)); q.dequeue(); q.dequeue(); assert(q.isEmpty() && q.getReferenceTime()==0); assert(q.enqueueScheduledEvent(EVENT_NOTE_ON,62,100,0xFFFFFFF0UL)); q.clear(); assert(q.isEmpty()); assert(q.enqueueScheduledEvent(EVENT_NOTE_ON,63,100,1)); }
static void note_sequencer_min_and_panic(){ resetCfg(); __test_millis=0; int fingerWrites=0, airWrites=0; FingerController fc([&](uint8_t,uint16_t,uint16_t){fingerWrites++;}); AirflowController ac([&](uint8_t,uint16_t,uint16_t){airWrites++;}); EventQueue q(8); NoteSequencer ns(q,fc,ac); ns.begin(); q.enqueueScheduledEvent(EVENT_NOTE_ON,60,100,0); ns.update(); assert(ns.getState()==STATE_POSITIONING && fingerWrites>0); __test_millis=10; ns.update(); assert(ns.getState()==STATE_PLAYING && ac.isValveOpen()); q.enqueueScheduledEvent(EVENT_NOTE_OFF,60,0,20); __test_millis=20; ns.update(); assert(ns.getState()==STATE_PLAYING); __test_millis=99; ns.update(); assert(ns.getState()==STATE_PLAYING); __test_millis=110; ns.update(); assert(ns.getState()==STATE_STOPPING); ns.stop(); assert(ns.getState()==STATE_IDLE && !ac.isValveOpen()); }

static void note_sequencer_monophonic_replacement(){ resetCfg(); cfg.minNoteDurationMs=100; cfg.minNoteIntervalForValveCloseMs=50; __test_millis=0; int fingerWrites=0; FingerController fc([&](uint8_t,uint16_t,uint16_t){fingerWrites++;}); AirflowController ac([&](uint8_t,uint16_t,uint16_t){}); EventQueue q(16); NoteSequencer ns(q,fc,ac); ns.begin(); q.enqueueScheduledEvent(EVENT_NOTE_ON,60,100,0); q.enqueueScheduledEvent(EVENT_NOTE_ON,62,100,0); q.enqueueScheduledEvent(EVENT_NOTE_OFF,60,0,0); q.enqueueScheduledEvent(EVENT_NOTE_OFF,62,0,0); ns.update(); assert(q.isEmpty()); assert(ns.getState()==STATE_IDLE); assert(fingerWrites>=2); ns.stop(); q.enqueueScheduledEvent(EVENT_NOTE_ON,60,100,1000); q.enqueueScheduledEvent(EVENT_NOTE_OFF,60,0,1010); q.enqueueScheduledEvent(EVENT_NOTE_ON,62,100,1010); __test_millis=1000; ns.update(); __test_millis=1010; ns.update(); assert(ns.getState()==STATE_POSITIONING); ns.stop(); q.enqueueScheduledEvent(EVENT_NOTE_ON,60,100,2000); q.enqueueScheduledEvent(EVENT_NOTE_ON,61,100,2000); q.enqueueScheduledEvent(EVENT_NOTE_ON,62,100,2000); q.enqueueScheduledEvent(EVENT_NOTE_OFF,60,0,2000); q.enqueueScheduledEvent(EVENT_NOTE_OFF,61,0,2000); q.enqueueScheduledEvent(EVENT_NOTE_OFF,62,0,2000); __test_millis=2000; ns.update(); assert(q.isEmpty()); assert(ns.getState()==STATE_IDLE); ns.stop(); q.enqueueScheduledEvent(EVENT_NOTE_ON,60,100,0xFFFFFFF0UL); __test_millis=0xFFFFFFF0UL; ns.update(); assert(ns.getState()==STATE_POSITIONING); ns.stop(); }

static void cc73_does_not_mutate_persistent_cfg(){ resetCfg(); uint8_t mode=cfg.airAttackMode; uint8_t off=cfg.airAttackOffset; AirflowController ac([](uint8_t,uint16_t,uint16_t){}); ac.setCC73Attack(100); assert(cfg.airAttackMode==mode); assert(cfg.airAttackOffset==off); assert(ac.runtimeAttackMode()==2); assert(ac.runtimeAttackOffset()>0); }
static void pca_detection_safe_boot(){ resetCfg(); extern WireClass Wire; extern int __pwm_write_count; __pwm_write_count=0; __digital_writes.clear(); Wire.clear(); InstrumentManager im0; assert(!im0.beginSafe()); assert(im0.hardwareInitStatus()==HW_PCA0_MISSING); assert(__digital_writes[PIN_SERVOS_OFF]==HIGH); assert(__pwm_write_count==0); Wire.setPresent(PCA_ADDR_BOARD0,true); InstrumentManager im1; assert(im1.beginSafe()); assert(im1.hardwareInitStatus()==HW_INIT_OK); assert(!im1.isSecondBoardEnabled()); im1.allSoundOff(); resetCfg(); cfg.fingers[0].pcaChannel=16; Wire.clear(); Wire.setPresent(PCA_ADDR_BOARD0,true); InstrumentManager im2; assert(!im2.beginSafe()); assert(im2.hardwareInitStatus()==HW_PCA1_MISSING); Wire.setPresent(PCA_ADDR_BOARD1,true); InstrumentManager im3; assert(im3.beginSafe()); assert(im3.isSecondBoardEnabled()); }
static void reservoir_autostart_behaviour(){ resetCfg(); extern WireClass Wire; Wire.clear(); Wire.setPresent(PCA_ADDR_BOARD0,true); cfg.airMode=AIR_MODE_PUMP_RESERVOIR; cfg.sensorType=SENSOR_TYPE_HALL_KY024; cfg.reservoirAutoStart=true; cfg.reservoirTargetPercent=60; InstrumentManager im; assert(im.beginSafe()); assert(im.getPressureCtrl().getTargetPercent()==60); im.allSoundOff(); assert(im.getPressureCtrl().getTargetPercent()==0); resetCfg(); Wire.clear(); Wire.setPresent(PCA_ADDR_BOARD0,true); cfg.airMode=AIR_MODE_PUMP_RESERVOIR; cfg.sensorType=SENSOR_TYPE_TOF_VL53L0X; cfg.reservoirAutoStart=true; cfg.reservoirTargetPercent=60; InstrumentManager im2; assert(im2.beginSafe()); assert(im2.getPressureCtrl().getTargetPercent()==0); }

static void fan_autonomous(){ resetCfg(); __test_millis=0; cfg.airMode=AIR_MODE_FAN_SERVO; cfg.fanPin=26; cfg.fanMinPwm=60; cfg.fanMaxPwm=200; cfg.fanIdlePercent=20; cfg.fanIdleTimeoutMs=100; FanController f; f.begin(); f.onNoteOn(); f.setSpeed(50); __test_millis=300; f.update(); assert(__analog_writes[26]==130); f.onNoteOff(); __test_millis+=50; f.update(); assert(__analog_writes[26] < 130 && __analog_writes[26] > 0); __test_millis+=100; f.update(); __test_millis+=300; f.update(); assert(__analog_writes[26]==0); f.stop(); assert(__analog_writes[26]==0); }
static void midi_validation_edges(){ resetCfg(); cfg.numNotes=1; cfg.notes[0].midiNote=0; assert(validateAndNormalizeConfig(cfg,nullptr).valid); resetCfg(); cfg.numNotes=1; cfg.notes[0].midiNote=127; assert(validateAndNormalizeConfig(cfg,nullptr).valid); resetCfg(); cfg.numNotes=1; cfg.notes[0].midiNote=128; assert(!validateAndNormalizeConfig(cfg,nullptr).valid); resetCfg(); cfg.notes[0].midiNote=200; assert(!validateAndNormalizeConfig(cfg,nullptr).valid); resetCfg(); cfg.notes[0].midiNote=(uint8_t)-1; assert(!validateAndNormalizeConfig(cfg,nullptr).valid); resetCfg(); cfg.numNotes=2; cfg.notes[0].midiNote=60; cfg.notes[1].midiNote=60; assert(!validateAndNormalizeConfig(cfg,nullptr).valid); }
static void air_modes_paths(){ for(int mode=0; mode<=5; ++mode){ resetCfg(); cfg.airMode=mode; cfg.valveType=(mode==1); cfg.motorType=MOTOR_TYPE_PWM; int writes=0; AirflowController ac([&](uint8_t,uint16_t,uint16_t){writes++;}); ac.begin(); bool initiallySafe = !ac.isValveOpen() || mode==AIR_MODE_SERVO_ONLY || mode==AIR_MODE_FAN_SERVO; assert(initiallySafe); ac.setAirflowForNote(60,100); ac.openValve(); assert(ac.isValveOpen()); ac.closeValve(); assert(!ac.isValveOpen()); ac.setAirflowToRest(); assert(writes>0); } }
// 1. Hz -> MIDI + cents conversion.
static void autocal_pitch_conversions(){
  assert(PitchMath::hzToMidi(440.0f)==69);
  assert(std::fabs(PitchMath::hzToCents(440.0f,69))<0.5f);
  assert(PitchMath::hzToMidi(880.0f)==81);
  assert(PitchMath::hzToMidi(261.626f)==60);
  float c=PitchMath::hzToCents(445.0f,69); assert(c>15.0f && c<25.0f);
  // 2-3. neighbour + octave relationships
  assert(PitchMath::isOctaveAbove(72,60)); assert(PitchMath::isOctaveAbove(84,60));
  assert(!PitchMath::isOctaveAbove(67,60)); assert(!PitchMath::isOctaveAbove(61,60)); assert(!PitchMath::isOctaveAbove(48,60));
  assert(PitchMath::isNoteMatch(60,-10.0f,60,50.0f));
  assert(!PitchMath::isNoteMatch(60,-60.0f,60,50.0f));
  assert(!PitchMath::isNoteMatch(61,0.0f,60,50.0f));
}

// 2-7. Pure aggregation helpers.
static void autocal_math_helpers(){
  using namespace AutoCalMath;
  // 5. dynamic threshold from noise
  assert(std::fabs(computeSoundThreshold(0.01f)-0.04f)<1e-6f);         // noise*ratio
  assert(std::fabs(computeSoundThreshold(0.0001f)-MIC_RMS_ABSOLUTE_MIN)<1e-6f); // floor wins
  // 6. median
  float a[5]={3,1,2,5,4}; assert(std::fabs(median(a,5)-3.0f)<1e-6f);
  float b[4]={1,2,3,4};   assert(std::fabs(median(b,4)-2.5f)<1e-6f);
  const float TH=0.012f, TOL=50.0f, MC=0.80f;
  AudioFrame good  = mkFrame(true,0.06f,60,-5.0f,0.95f);
  AudioFrame neigh = mkFrame(true,0.06f,61, 0.0f,0.95f);
  AudioFrame oct   = mkFrame(true,0.06f,72, 0.0f,0.95f);
  AudioFrame lowc  = mkFrame(true,0.06f,60, 0.0f,0.50f);
  AudioFrame quiet = mkFrame(true,0.005f,60,0.0f,0.95f);
  AudioFrame sharp = mkFrame(true,0.06f,60,-70.0f,0.95f);
  // 4. exact note accepted
  assert(frameIsNote(good,60,TH,TOL,MC));
  // 2. neighbour rejected  3. octave rejected (and flagged overblow)
  assert(!frameIsNote(neigh,60,TH,TOL,MC));
  assert(!frameIsNote(oct,60,TH,TOL,MC));
  assert(frameIsOverblow(oct,60,TH,MC));
  // low confidence / near-noise / wide-cents rejected
  assert(!frameIsNote(lowc,60,TH,TOL,MC));
  assert(!frameIsNote(quiet,60,TH,TOL,MC));
  assert(!frameIsNote(sharp,60,TH,TOL,MC));
  // 7. required valid frames (4 of 5)
  AudioFrame f4[5]={good,good,good,good,neigh};
  PositionStats s4=evaluatePosition(f4,5,60,TH,TOL,MC);
  assert(s4.validFrames==4 && positionValid(s4,AUTOCAL_REQUIRED_VALID_FRAMES));
  AudioFrame f3[5]={good,good,good,neigh,neigh};
  PositionStats s3=evaluatePosition(f3,5,60,TH,TOL,MC);
  assert(s3.validFrames==3 && !positionValid(s3,AUTOCAL_REQUIRED_VALID_FRAMES));
  AudioFrame fo[5]={oct,oct,oct,good,good};
  PositionStats so=evaluatePosition(fo,5,60,TH,TOL,MC);
  assert(so.overblowFrames==3 && positionOverblown(so));
  // 9. nominal computation + relationship clamping
  assert(computeNominalPercent(20,68,0.40f)==39);
  assert(computeNominalPercent(0,100,0.40f)==40);
  assert(clampNominal(20,60,10,5)==25);   // below min+guard
  assert(clampNominal(20,60,70,5)==55);   // above max-guard
  assert(clampNominal(20,60,40,5)==40);   // inside band untouched
  assert(snrDb(0.06f,0.003f)>20.0f);
  assert(std::fabs(pitchStability(0.0f,100.0f)-1.0f)<1e-6f);
  assert(std::fabs(pitchStability(100.0f,100.0f)-0.0f)<1e-6f);
}

// 10-11. Nominal migration formula + validator relationship (0<=min<=nominal<=max<=100).
static void autocal_config_nominal_validation(){
  // 10. migration: nominal derived from min/max == min + 0.40*(max-min)
  uint8_t mn=20, mx=70; uint8_t nom=(uint8_t)(mn + (2*(mx-mn))/5);
  assert(nom==40);
  resetCfg(); cfg.numNotes=1; cfg.notes[0].midiNote=60;
  cfg.notes[0].airflowMinPercent=20; cfg.notes[0].airflowMaxPercent=70; cfg.notes[0].airflowNominalPercent=40;
  assert(validateAndNormalizeConfig(cfg,nullptr).valid);
  // 11a. nominal < min rejected
  cfg.notes[0].airflowNominalPercent=10; assert(!validateAndNormalizeConfig(cfg,nullptr).valid);
  // 11b. nominal > max rejected
  resetCfg(); cfg.numNotes=1; cfg.notes[0].midiNote=60;
  cfg.notes[0].airflowMinPercent=20; cfg.notes[0].airflowMaxPercent=70; cfg.notes[0].airflowNominalPercent=80;
  assert(!validateAndNormalizeConfig(cfg,nullptr).valid);
}

// 8-9. Integration: detect min/max band and a nominal inside it from simulated audio.
static void autocal_integration_minmax_nominal(){
  resetCfg(); cfg.numNotes=1; cfg.notes[0].midiNote=60;
  cfg.servoAirflowMin=60; cfg.servoAirflowMax=100;
  __test_millis=0;
  FakeAudio fa;
  FingerController fc([](uint8_t,uint16_t,uint16_t){});
  AirflowController ac([](uint8_t,uint16_t,uint16_t){}); ac.begin();
  FakeAirSupply as; AutoCalibrator cal(fc,ac,fa,as);
  cal.start(ACAL_MODE_AIRFLOW);
  assert(cal.isRunning());
  driveAutoCal(cal, fa, [](int pct,int note,FakeAudio& f){ bandModel(pct,60,20,60,f); });
  assert(cal.isComplete());
  AutoCalNoteResult r=cal.getResult(0);
  assert(r.valid);
  assert(r.airMin>=16 && r.airMin<=24);
  assert(r.airMax>=56 && r.airMax<=64);
  assert(r.airNominal>=r.airMin && r.airNominal<=r.airMax);
  assert(r.confidence>0 && r.confidence<=100);
  assert(r.signalToNoiseRatio>0.0f);
}

// 13. A failed note must not overwrite a previously valid calibration.
static void autocal_keep_old_on_fail(){
  resetCfg(); cfg.numNotes=2; cfg.notes[0].midiNote=60; cfg.notes[1].midiNote=62;
  cfg.servoAirflowMin=60; cfg.servoAirflowMax=100;
  cfg.notes[0].airflowMinPercent=0; cfg.notes[0].airflowMaxPercent=100; cfg.notes[0].airflowNominalPercent=40;
  cfg.notes[1].airflowMinPercent=11; cfg.notes[1].airflowMaxPercent=77; cfg.notes[1].airflowNominalPercent=33;
  __test_millis=0;
  FakeAudio fa;
  FingerController fc([](uint8_t,uint16_t,uint16_t){});
  AirflowController ac([](uint8_t,uint16_t,uint16_t){}); ac.begin();
  FakeAirSupply as; AutoCalibrator cal(fc,ac,fa,as);
  cal.start(ACAL_MODE_AIRFLOW);
  // Only note 0 (index 0) ever sounds; note 1 stays silent -> fails.
  driveAutoCal(cal, fa, [](int pct,int note,FakeAudio& f){ if(note==0) bandModel(pct,60,20,60,f); else bandModel(pct,62,200,201,f); });
  assert(cal.isComplete());
  assert(cal.getResult(0).valid);
  assert(!cal.getResult(1).valid);
  cal.applyResults();
  // Note 1 keeps its old values; note 0 was overwritten by the valid result.
  assert(cfg.notes[1].airflowMinPercent==11 && cfg.notes[1].airflowMaxPercent==77 && cfg.notes[1].airflowNominalPercent==33);
  assert(cfg.notes[0].airflowMinPercent<=cfg.notes[0].airflowNominalPercent && cfg.notes[0].airflowNominalPercent<=cfg.notes[0].airflowMaxPercent);
}

// 12. Global timeout aborts safely (valve closed, one-shot timeout event).
static void autocal_timeout_safe_stop(){
  resetCfg(); cfg.numNotes=1; cfg.notes[0].midiNote=60; cfg.servoAirflowMin=60; cfg.servoAirflowMax=100;
  __test_millis=0;
  FakeAudio fa;
  FingerController fc([](uint8_t,uint16_t,uint16_t){});
  AirflowController ac([](uint8_t,uint16_t,uint16_t){}); ac.begin();
  FakeAirSupply as; AutoCalibrator cal(fc,ac,fa,as);
  cal.start(ACAL_MODE_AIRFLOW);
  // Run past noise so the valve is open, keeping the note silent with fresh frames.
  for(int i=0;i<40 && cal.isRunning();i++){ fa.rms=0.003f; fa.valid=false; fa.midi=0; fa.hz=0; fa.seq++; fa.ts=__test_millis; __test_millis+=25; cal.update(); }
  assert(cal.isRunning());
  // Jump well past the (note-count scaled) global timeout ceiling.
  __test_millis += (unsigned long)AUTOCAL_GLOBAL_TIMEOUT_MS + 1000;
  cal.update();
  assert(!cal.isRunning());
  assert(cal.takeTimeoutEvent());
  assert(!cal.takeTimeoutEvent());   // one-shot: cleared after read
  assert(!ac.isValveOpen());
}

// §3. A frozen audio source (sequence never advances) must not validate a note.
static void autocal_frozen_source_fails(){
  resetCfg(); cfg.numNotes=1; cfg.notes[0].midiNote=60; cfg.servoAirflowMin=60; cfg.servoAirflowMax=100;
  cfg.notes[0].airflowMinPercent=5; cfg.notes[0].airflowMaxPercent=95; cfg.notes[0].airflowNominalPercent=40;
  __test_millis=0;
  FakeAudio fa; fa.seq=7;  // constant sequence for the whole run
  FingerController fc([](uint8_t,uint16_t,uint16_t){});
  AirflowController ac([](uint8_t,uint16_t,uint16_t){}); ac.begin();
  FakeAirSupply as; AutoCalibrator cal(fc,ac,fa,as);
  cal.start(ACAL_MODE_AIRFLOW);
  // Source always "sounds" the right note but never produces a fresh frame.
  driveAutoCal(cal, fa, [](int,int,FakeAudio& f){ f.rms=0.06f; f.valid=true; f.conf=0.95f; f.midi=60; f.hz=PitchMath::midiToHz(60); f.cents=0.0f; f.snd=true; }, /*freshFrames=*/false);
  assert(cal.isComplete());
  AutoCalNoteResult r=cal.getResult(0);
  assert(!r.valid);
  assert(r.failureReason==ACAL_FAIL_AUDIO_STALE);
  // Old config preserved.
  cal.applyResults();
  assert(cfg.notes[0].airflowNominalPercent==40);
}

// §7. The air source (pump/fan/reservoir) must be ready before anything is measured.
// (a) A source that never becomes ready aborts the run: every note fails with
//     ACAL_FAIL_AIR_SUPPLY, nothing is written, and the source is stopped safely.
// (b) A source that becomes ready only after a delay simply makes the calibrator
//     wait, then the run proceeds and succeeds normally.
static void autocal_air_supply_gate(){
  // (a) never ready
  resetCfg(); cfg.numNotes=2; cfg.notes[0].midiNote=60; cfg.notes[1].midiNote=61;
  cfg.servoAirflowMin=60; cfg.servoAirflowMax=120;
  for(int i=0;i<2;i++){ cfg.notes[i].airflowMinPercent=5; cfg.notes[i].airflowMaxPercent=95;
    cfg.notes[i].airflowNominalPercent=40; }
  __test_millis=0;
  { FakeAudio fa;
    FingerController fc([](uint8_t,uint16_t,uint16_t){});
    AirflowController ac([](uint8_t,uint16_t,uint16_t){}); ac.begin();
    FakeAirSupply as; as.readyAfter=0xFFFFFFFFu;  // never becomes ready
    AutoCalibrator cal(fc,ac,fa,as);
    cal.start(ACAL_MODE_AIRFLOW);
    driveAutoCal(cal, fa, [](int pct,int note,FakeAudio& f){ bandModel(pct,60+note,20,60,f); });
    assert(cal.isComplete());
    assert(as.stopped);                        // source returned to a safe state
    for(int i=0;i<2;i++){ AutoCalNoteResult r=cal.getResult(i);
      assert(!r.valid); assert(r.failureReason==ACAL_FAIL_AIR_SUPPLY); }
    AutoCalApplyResult ap=cal.applyResults();
    assert(!ap.applied && ap.validCount==0);
    assert(cfg.notes[0].airflowNominalPercent==40); // nothing written
  }
  // (b) ready only after a delay: the calibrator waits, then a normal run succeeds
  resetCfg(); cfg.numNotes=1; cfg.notes[0].midiNote=60;
  cfg.servoAirflowMin=60; cfg.servoAirflowMax=120;
  cfg.notes[0].airflowMinPercent=0; cfg.notes[0].airflowMaxPercent=100; cfg.notes[0].airflowNominalPercent=40;
  __test_millis=0;
  { FakeAudio fa;
    FingerController fc([](uint8_t,uint16_t,uint16_t){});
    AirflowController ac([](uint8_t,uint16_t,uint16_t){}); ac.begin();
    FakeAirSupply as; as.readyAfter=10;         // ready after 10 poll cycles
    AutoCalibrator cal(fc,ac,fa,as);
    cal.start(ACAL_MODE_AIRFLOW);
    driveAutoCal(cal, fa, [](int pct,int,FakeAudio& f){ bandModel(pct,60,20,60,f); });
    assert(cal.isComplete());
    assert(cal.getResult(0).valid);             // gate waited, then proceeded
    assert(as.lastDemand==100);                 // representative demand was asserted
  }
}

// §4. Fourteen well-behaved notes complete without hitting a timeout.
static void autocal_14_notes_no_timeout(){
  resetCfg();
  cfg.numNotes=14; cfg.servoAirflowMin=60; cfg.servoAirflowMax=120;
  for(int i=0;i<14;i++){ cfg.notes[i].midiNote=(uint8_t)(60+i);
    cfg.notes[i].airflowMinPercent=0; cfg.notes[i].airflowMaxPercent=100; cfg.notes[i].airflowNominalPercent=40; }
  __test_millis=0;
  FakeAudio fa;
  FingerController fc([](uint8_t,uint16_t,uint16_t){});
  AirflowController ac([](uint8_t,uint16_t,uint16_t){}); ac.begin();
  FakeAirSupply as; AutoCalibrator cal(fc,ac,fa,as);
  cal.start(ACAL_MODE_AIRFLOW);
  // Each note sounds its own expected pitch in the 20..60% band.
  driveAutoCal(cal, fa, [](int pct,int note,FakeAudio& f){ bandModel(pct, 60+note, 20, 60, f); });
  assert(cal.isComplete());   // finished, not aborted by a timeout
  assert(!cal.takeTimeoutEvent());
  int okCount=0; for(int i=0;i<14;i++) if(cal.getResult(i).valid) okCount++;
  assert(okCount==14);
}

// §5. A note constantly +70 cents off is refused (coarse may see it, result fails,
// nothing written to persistent config).
static void autocal_plus70_cents_rejected(){
  resetCfg(); cfg.numNotes=1; cfg.notes[0].midiNote=60; cfg.servoAirflowMin=60; cfg.servoAirflowMax=120;
  cfg.notes[0].airflowMinPercent=7; cfg.notes[0].airflowMaxPercent=88; cfg.notes[0].airflowNominalPercent=44;
  __test_millis=0;
  FakeAudio fa;
  FingerController fc([](uint8_t,uint16_t,uint16_t){});
  AirflowController ac([](uint8_t,uint16_t,uint16_t){}); ac.begin();
  FakeAirSupply as; AutoCalibrator cal(fc,ac,fa,as);
  cal.start(ACAL_MODE_AIRFLOW);
  // Correct MIDI note but always +70 cents (inside onset tol 80, outside stable 50).
  driveAutoCal(cal, fa, [](int pct,int,FakeAudio& f){
    if(pct>=20 && pct<=60){ f.rms=0.06f; f.valid=true; f.conf=0.95f; f.midi=60; f.hz=PitchMath::midiToHz(60); f.cents=70.0f; f.snd=true; }
    else { f.rms=0.003f; f.valid=false; f.conf=0.0f; f.midi=0; f.hz=0; f.cents=0; f.snd=false; }
  });
  assert(cal.isComplete());
  AutoCalNoteResult r=cal.getResult(0);
  assert(!r.valid);
  assert(r.failureReason==ACAL_FAIL_NO_STABLE_NOMINAL || r.failureReason==ACAL_FAIL_WRONG_NOTE);
  // Apply must not write anything and must not claim a save.
  AutoCalApplyResult ap=cal.applyResults();
  assert(ap.validCount==0 && !ap.applied && !ap.saved);
  assert(cfg.notes[0].airflowMinPercent==7 && cfg.notes[0].airflowMaxPercent==88 && cfg.notes[0].airflowNominalPercent==44);
}

// §6. Range finder: multi-frame sweep over a safe angle window finds min/max.
static void autocal_range_finder(){
  resetCfg(); cfg.numNotes=3; cfg.notes[0].midiNote=60; cfg.notes[1].midiNote=62; cfg.notes[2].midiNote=64;
  cfg.servoAirflowMin=60; cfg.servoAirflowMax=120;
  __test_millis=0;
  FakeAudio fa;
  FingerController fc([](uint8_t,uint16_t,uint16_t){});
  AirflowController ac([](uint8_t,uint16_t,uint16_t){}); ac.begin();
  FakeAirSupply as; AutoCalibrator cal(fc,ac,fa,as);
  cal.start(ACAL_MODE_RANGE_FIND);
  assert(cal.isRunning());
  assert(cal.isRangeMode());
  int expMidi = cfg.notes[cfg.numNotes/2].midiNote;  // middle note = 62
  int it=0;
  while(cal.isRunning() && it++<200000){
    int ang = cal.getCurrentAngle();
    if(ang>=60 && ang<=120){ fa.rms=0.06f; fa.valid=true; fa.conf=0.95f; fa.midi=expMidi; fa.hz=PitchMath::midiToHz(expMidi); fa.cents=-3.0f; fa.snd=true; }
    else { fa.rms=0.003f; fa.valid=false; fa.conf=0.0f; fa.midi=0; fa.hz=0; fa.cents=0; fa.snd=false; }
    fa.seq++; fa.ts=__test_millis;
    __test_millis+=25; cal.update();
  }
  assert(cal.isRangeFinderComplete());
  int mn=cal.getRangeFinderMin(), mx=cal.getRangeFinderMax();
  assert(mn>=54 && mn<=63);    // ~60 deg minus safety margin
  assert(mx>=117 && mx<=129);  // ~120 deg plus safety margin
  assert(mn>=AUTOCAL_RF_MIN_SAFE_ANGLE && mx<=AUTOCAL_RF_MAX_SAFE_ANGLE);  // within safe window
}

// D3. A frozen audio source during the range finder must terminate it in
// ACAL_RF_COMPLETE with invalid angles and an audio-stale reason - never loop on
// the centre note nor fall through to ACAL_COMPLETE (airflow result handling).
static void autocal_range_finder_stale(){
  resetCfg(); cfg.numNotes=3; cfg.notes[0].midiNote=60; cfg.notes[1].midiNote=62; cfg.notes[2].midiNote=64;
  cfg.servoAirflowMin=60; cfg.servoAirflowMax=120;
  __test_millis=0;
  FakeAudio fa; fa.seq=5;  // frozen sequence
  FingerController fc([](uint8_t,uint16_t,uint16_t){});
  AirflowController ac([](uint8_t,uint16_t,uint16_t){}); ac.begin();
  FakeAirSupply as; AutoCalibrator cal(fc,ac,fa,as);
  cal.start(ACAL_MODE_RANGE_FIND);
  driveAutoCal(cal, fa, [](int,int,FakeAudio& f){
    f.rms=0.06f; f.valid=true; f.conf=0.9f; f.midi=62; f.hz=PitchMath::midiToHz(62); f.cents=0.0f; f.snd=true;
  }, /*freshFrames=*/false);
  assert(cal.isRangeFinderComplete());
  assert(!cal.isComplete());
  assert(cal.getRangeFinderMin() < 0 && cal.getRangeFinderMax() < 0);
  assert(cal.getRangeFailureReason() == ACAL_FAIL_AUDIO_STALE);
}

// D5. Range-finder apply is storage-checked: a LittleFS failure restores the old
// angles and reports applied=false; a successful save writes the discovered ones.
static void autocal_range_apply_storage(){
  extern bool __config_save_result;
  resetCfg(); cfg.numNotes=3; cfg.notes[0].midiNote=60; cfg.notes[1].midiNote=62; cfg.notes[2].midiNote=64;
  cfg.servoAirflowMin=60; cfg.servoAirflowMax=120;
  __test_millis=0;
  FakeAudio fa;
  FingerController fc([](uint8_t,uint16_t,uint16_t){});
  AirflowController ac([](uint8_t,uint16_t,uint16_t){}); ac.begin();
  FakeAirSupply as; AutoCalibrator cal(fc,ac,fa,as);
  cal.start(ACAL_MODE_RANGE_FIND);
  int expMidi=62, it=0;
  while(cal.isRunning() && it++<200000){
    int ang=cal.getCurrentAngle();
    if(ang>=60 && ang<=120){ fa.rms=0.06f;fa.valid=true;fa.conf=0.95f;fa.midi=expMidi;fa.hz=PitchMath::midiToHz(expMidi);fa.cents=-3.0f;fa.snd=true; }
    else { fa.rms=0.003f;fa.valid=false;fa.conf=0;fa.midi=0;fa.hz=0;fa.cents=0;fa.snd=false; }
    fa.seq++; fa.ts=__test_millis; __test_millis+=25; cal.update();
  }
  assert(cal.isRangeFinderComplete());
  int mn=cal.getRangeFinderMin(), mx=cal.getRangeFinderMax();
  assert(mn>=0 && mx>=0);
  uint16_t oldMin=cfg.servoAirflowMin, oldMax=cfg.servoAirflowMax;
  __config_save_result=false;                 // simulate LittleFS failure
  RangeApplyResult r1=cal.applyRangeResults();
  __config_save_result=true;
  assert(!r1.applied && !r1.saved);
  assert(cfg.servoAirflowMin==oldMin && cfg.servoAirflowMax==oldMax);  // rolled back
  RangeApplyResult r2=cal.applyRangeResults();
  assert(r2.applied && r2.saved);
  assert((int)cfg.servoAirflowMin==mn && (int)cfg.servoAirflowMax==mx);
}

// D9. A mid-note air-source drop-out is reported as ACAL_FAIL_AIR_SUPPLY (with the
// specific sub-error), not misclassified as no-sound / wrong-note.
static void autocal_air_lost_midnote(){
  resetCfg(); cfg.numNotes=1; cfg.notes[0].midiNote=60; cfg.servoAirflowMin=60; cfg.servoAirflowMax=120;
  cfg.notes[0].airflowMinPercent=0; cfg.notes[0].airflowMaxPercent=100; cfg.notes[0].airflowNominalPercent=40;
  __test_millis=0;
  FakeAudio fa;
  FingerController fc([](uint8_t,uint16_t,uint16_t){});
  AirflowController ac([](uint8_t,uint16_t,uint16_t){}); ac.begin();
  FakeAirSupply as; AutoCalibrator cal(fc,ac,fa,as);
  cal.start(ACAL_MODE_AIRFLOW);
  int it=0; bool dropped=false;
  while(cal.isRunning() && it++<200000){
    bandModel(cal.getCurrentAirPercent(),60,20,60,fa);
    fa.seq++; fa.ts=__test_millis;
    if(!dropped && strcmp(cal.getPhaseName(),"coarse")==0){
      as.forceNotReady=true; as.dropError=CAL_AIR_FAULT; dropped=true;
    }
    __test_millis+=25; cal.update();
  }
  assert(dropped);
  assert(cal.isComplete());
  AutoCalNoteResult r=cal.getResult(0);
  assert(!r.valid);
  assert(r.failureReason==ACAL_FAIL_AIR_SUPPLY);
  assert(cal.getAirSupplyError()==CAL_AIR_FAULT);
}

// D8. Reservoir mode with no usable sensor is refused (strict, not best-effort):
// the pump is stopped, the supply is never ready, and it reports sensor_fault.
static void calair_reservoir_requires_sensor(){
  resetCfg(); cfg.airMode=AIR_MODE_PUMP_RESERVOIR; cfg.reservoirTargetPercent=60;
  PressureController pc; pc._sensorDetected=false;   // no sensor
  FanController fan;
  CalibrationAirSupply sup(pc, fan);
  sup.prepare();
  assert(!sup.isReady());
  assert(sup.getError()==CAL_AIR_SENSOR_FAULT);
  assert(!pc.isPumpRunning());
}

// D2. During an actuator session the idle power-down is inhibited so the PCA9685
// OE / servos are never cut mid-measurement; normal management resumes after.
static void instrument_power_held_during_actuator_session(){
  extern std::map<uint8_t,int> __digital_writes;
  resetCfg(); extern WireClass Wire; Wire.clear(); Wire.setPresent(PCA_ADDR_BOARD0,true);
  cfg.timeUnpower=200; __test_millis=0;
  InstrumentManager im; assert(im.beginSafe());
  im.setActuatorSessionActive(true);           // begins a calibration/range-finder session
  assert(im.isActuatorSessionActive());
  assert(__digital_writes[PIN_SERVOS_OFF]==LOW);   // powered immediately
  for(int i=0;i<20;i++){ __test_millis+=100; im.update(); }
  assert(__digital_writes[PIN_SERVOS_OFF]==LOW);   // still powered despite long idle
  im.setActuatorSessionActive(false);          // session ends
  for(int i=0;i<20;i++){ __test_millis+=100; im.update(); }
  assert(__digital_writes[PIN_SERVOS_OFF]==HIGH);  // normal idle power-down resumes
  im.allSoundOff();
}

// Review #2. Central calibration exclusivity: while an actuator session is active,
// external MIDI (BLE/rtp/DIN/file player) must not drive the sequencer/actuators.
static void instrument_ignores_midi_during_calibration(){
  resetCfg(); extern WireClass Wire; Wire.clear(); Wire.setPresent(PCA_ADDR_BOARD0,true);
  cfg.numNotes=1; cfg.notes[0].midiNote=60; __test_millis=0;
  InstrumentManager im; assert(im.beginSafe());
  im.setActuatorSessionActive(true);
  im.noteOn(60,100); im.handleControlChange(7,100);
  for(int i=0;i<4;i++){ __test_millis+=20; im.update(); }
  assert(im.getSequencer().getState()==STATE_IDLE);       // MIDI ignored during session
  im.setActuatorSessionActive(false);                     // session ends
  im.noteOn(60,100);
  im.update();
  assert(im.getSequencer().getState()==STATE_POSITIONING); // MIDI drives again
  im.allSoundOff();
}

// Audit P0.2. After a failed safe init (PCA missing), every actuator path is inert:
// no PWM writes, OE stays disabled, the sequencer never runs.
static void instrument_inert_after_pca_failure(){
  extern std::map<uint8_t,int> __digital_writes; extern int __pwm_write_count;
  resetCfg(); extern WireClass Wire; Wire.clear();   // PCA0 absent
  cfg.numNotes=1; cfg.notes[0].midiNote=60; __test_millis=0;
  __pwm_write_count=0; __digital_writes.clear();
  InstrumentManager im;
  assert(!im.beginSafe());
  assert(im.hardwareInitStatus()==HW_PCA0_MISSING);
  assert(!im.isHardwareReady());
  assert(__digital_writes[PIN_SERVOS_OFF]==HIGH);   // OE never enabled
  assert(__pwm_write_count==0);                     // no channel programmed
  // Every actuator path must be a no-op.
  im.noteOn(60,100); im.noteOff(60); im.handleControlChange(7,100);
  for(int i=0;i<5;i++){ __test_millis+=50; im.update(); }
  im.setPWM(0,0,300);
  im.powerOnServos();
  assert(__pwm_write_count==0);
  assert(__digital_writes[PIN_SERVOS_OFF]==HIGH);   // still disabled
  assert(im.getSequencer().getState()==STATE_IDLE);
}

// Review #14. The global timeout scales to the largest note count instead of being
// clamped below the sum of the per-note budgets (which happened ~24 notes before).
static void autocal_global_timeout_scales_to_max_notes(){
  resetCfg(); cfg.numNotes=MAX_NOTES;
  for(int i=0;i<MAX_NOTES;i++) cfg.notes[i].midiNote=(uint8_t)(i & 0x7F);
  __test_millis=0;
  FakeAudio fa;
  FingerController fc([](uint8_t,uint16_t,uint16_t){});
  AirflowController ac([](uint8_t,uint16_t,uint16_t){}); ac.begin();
  FakeAirSupply as; AutoCalibrator cal(fc,ac,fa,as);
  cal.start(ACAL_MODE_AIRFLOW);
  unsigned long expected=(unsigned long)MAX_NOTES*AUTOCAL_TIMEOUT_PER_NOTE_MS + AUTOCAL_GLOBAL_MARGIN_MS;
  assert(cal.getGlobalTimeoutMs()==expected);
  assert(cal.getGlobalTimeoutMs() >= (unsigned long)cfg.numNotes*AUTOCAL_TIMEOUT_PER_NOTE_MS);
  cal.stop();
}

// Review #3. CC2 (breath) silence must not be force-reopened, and live CC must
// recompute the held note: setAirflowForNote returns whether the note should sound,
// and recomputeActiveNote() opens/closes the valve as breath crosses the threshold.
static void airflow_cc2_silence_and_live_cc(){
  resetCfg();
  cfg.airMode=AIR_MODE_SOLENOID_SERVO;   // physical valve
  cfg.cc2Enabled=true; cfg.cc2SilenceThreshold=10; cfg.cc2ResponseCurve=1.0f; cfg.cc2TimeoutMs=0;
  cfg.numNotes=1; cfg.notes[0].midiNote=60;
  cfg.notes[0].airflowMinPercent=10; cfg.notes[0].airflowMaxPercent=90; cfg.notes[0].airflowNominalPercent=50;
  cfg.servoAirflowMin=30; cfg.servoAirflowMax=150; cfg.servoAirflowOff=20;
  __test_millis=1;
  AirflowController ac([](uint8_t,uint16_t,uint16_t){}); ac.begin();
  // Breath below threshold at the onset: the note is held but must NOT sound, and
  // the caller (sequencer) must not open the valve over this silence.
  for(int i=0;i<CC2_SMOOTHING_BUFFER_SIZE;i++) ac.updateCC2Breath(2);
  assert(!ac.setAirflowForNote(60,100));
  assert(ac.isNoteActive() && !ac.isValveOpen());
  // Breath rises: recompute resumes the note and opens the valve.
  for(int i=0;i<CC2_SMOOTHING_BUFFER_SIZE;i++) ac.updateCC2Breath(110);
  assert(ac.recomputeActiveNote());
  assert(ac.isValveOpen());
  // Breath drops again: recompute silences the note and closes the valve.
  for(int i=0;i<CC2_SMOOTHING_BUFFER_SIZE;i++) ac.updateCC2Breath(1);
  assert(!ac.recomputeActiveNote());
  assert(!ac.isValveOpen());
  // Note end clears the active state (live CC can no longer resurrect it).
  ac.setAirflowToRest();
  assert(!ac.isNoteActive());
  assert(!ac.recomputeActiveNote());
}

// Audit P0.4. An interrupted attack must not keep driving a play angle after the
// return to rest - in valveless modes that would restore airflow after a Note Off.
static void airflow_attack_cancelled_on_rest(){
  resetCfg();
  cfg.airMode=AIR_MODE_SERVO_ONLY;   // valveless: the airflow servo IS the flow
  cfg.airAttackMs=200; cfg.cc2Enabled=false;
  cfg.numNotes=1; cfg.notes[0].midiNote=60;
  cfg.notes[0].airflowMinPercent=10; cfg.notes[0].airflowMaxPercent=90; cfg.notes[0].airflowNominalPercent=50;
  cfg.servoAirflowMin=30; cfg.servoAirflowMax=150; cfg.servoAirflowOff=20;
  __test_millis=0;
  AirflowController ac([](uint8_t,uint16_t,uint16_t){}); ac.begin();
  ac.setCC73Attack(120);              // crescendo (offset > 0) -> attack arms
  ac.openValve();                     // servo-only: marks sounding (_solenoidOpen)
  ac.setAirflowForNote(60,100);
  assert(ac.isNoteActive());
  __test_millis=60; ac.update();      // mid-attack, sounding above rest
  assert(ac.getAirflowAngle() > cfg.servoAirflowOff);
  // Note off BEFORE the attack completes.
  ac.closeValve();
  ac.setAirflowToRest();
  assert(ac.getAirflowAngle()==cfg.servoAirflowOff);
  // Further updates must NOT re-drive a play angle (attack fully cancelled).
  for(int i=0;i<12;i++){ __test_millis+=30; ac.update(); }
  assert(ac.getAirflowAngle()==cfg.servoAirflowOff);
}

// Audit P0.5. When the CC2 (breath) stream stops during a held note, update() must
// fall back to velocity once (no CC event arrives to trigger it otherwise).
static void airflow_cc2_timeout_on_held_note(){
  resetCfg();
  cfg.airMode=AIR_MODE_SOLENOID_SERVO;
  cfg.cc2Enabled=true; cfg.cc2SilenceThreshold=10; cfg.cc2ResponseCurve=1.0f; cfg.cc2TimeoutMs=500;
  cfg.numNotes=1; cfg.notes[0].midiNote=60;
  cfg.notes[0].airflowMinPercent=10; cfg.notes[0].airflowMaxPercent=90; cfg.notes[0].airflowNominalPercent=50;
  cfg.servoAirflowMin=30; cfg.servoAirflowMax=150; cfg.servoAirflowOff=20;
  __test_millis=1;
  AirflowController ac([](uint8_t,uint16_t,uint16_t){}); ac.begin();
  for(int i=0;i<CC2_SMOOTHING_BUFFER_SIZE;i++) ac.updateCC2Breath(4);   // breath below threshold => silent
  assert(!ac.setAirflowForNote(60,100));
  ac.closeValve();                    // silent onset: valve stays closed
  assert(!ac.isValveOpen());
  // Breath stream stops (no more CC2). Time crosses cc2TimeoutMs: fall back to velocity.
  bool opened=false;
  for(int i=0;i<40 && !opened;i++){ __test_millis+=50; ac.update(); if(ac.isValveOpen()) opened=true; }
  assert(opened);
  assert(ac.getAirflowAngle() > cfg.servoAirflowOff);   // now sounding at the velocity level
}

// §11/§16. LittleFS save failure: results applied in RAM then restored, no false success.
static void autocal_storage_failure_restores(){
  extern bool __config_save_result;
  resetCfg(); cfg.numNotes=1; cfg.notes[0].midiNote=60; cfg.servoAirflowMin=60; cfg.servoAirflowMax=120;
  cfg.notes[0].airflowMinPercent=1; cfg.notes[0].airflowMaxPercent=99; cfg.notes[0].airflowNominalPercent=50;
  __test_millis=0;
  FakeAudio fa;
  FingerController fc([](uint8_t,uint16_t,uint16_t){});
  AirflowController ac([](uint8_t,uint16_t,uint16_t){}); ac.begin();
  FakeAirSupply as; AutoCalibrator cal(fc,ac,fa,as);
  cal.start(ACAL_MODE_AIRFLOW);
  driveAutoCal(cal, fa, [](int pct,int,FakeAudio& f){ bandModel(pct,60,20,60,f); });
  assert(cal.isComplete());
  assert(cal.getResult(0).valid);
  __config_save_result=false;             // simulate LittleFS failure
  AutoCalApplyResult ap=cal.applyResults();
  __config_save_result=true;              // restore for other tests
  // On a storage failure the RAM config is rolled back, so NOTHING is in effect:
  // applied must be false too (the client must never be told a partial success).
  assert(!ap.applied && !ap.saved && ap.validCount==1);
  // RAM restored to the previous configuration (no partial write left behind).
  assert(cfg.notes[0].airflowMinPercent==1 && cfg.notes[0].airflowMaxPercent==99 && cfg.notes[0].airflowNominalPercent==50);
}

// 15. Starting calibration with no microphone is a safe no-op.
static void autocal_mic_absent(){
  resetCfg(); cfg.numNotes=1; cfg.notes[0].midiNote=60;
  FakeAudio fa; fa.micDetected=false;
  FingerController fc([](uint8_t,uint16_t,uint16_t){});
  AirflowController ac([](uint8_t,uint16_t,uint16_t){}); ac.begin();
  FakeAirSupply as; AutoCalibrator cal(fc,ac,fa,as);
  cal.start(ACAL_MODE_AIRFLOW);
  assert(!cal.isRunning());
  assert(!cal.isComplete());
  assert(!ac.isValveOpen());
}

// §2. The calibrated nominal airflow really drives the produced angle.
static void airflow_nominal_drives_angle(){
  resetCfg();
  cfg.numNotes=1; cfg.notes[0].midiNote=60;
  cfg.notes[0].airflowMinPercent=0; cfg.notes[0].airflowMaxPercent=100;
  cfg.servoAirflowMin=60; cfg.servoAirflowMax=120; cfg.servoAirflowOff=20;
  cfg.airVelocityResponse=100; cfg.ccVolumeDefault=127; cfg.ccExpressionDefault=127;
  cfg.cc2Enabled=false; cfg.ccModulationDefault=0; cfg.airAttackMode=0;
  uint16_t lastOff=0; bool got=false;
  AirflowController ac([&](uint8_t ch,uint16_t,uint16_t off){ if(ch==cfg.airflowPcaChannel){lastOff=off;got=true;} });
  ac.begin();
  // Same note, same (pivot) velocity 64 -> should land on the nominal angle.
  cfg.notes[0].airflowNominalPercent=20; got=false; ac.setAirflowForNote(60,64);
  assert(got); uint16_t lowNom=lastOff;
  cfg.notes[0].airflowNominalPercent=80; got=false; ac.setAirflowForNote(60,64);
  assert(got); uint16_t highNom=lastOff;
  assert(highNom>lowNom);   // higher nominal -> larger airflow angle -> larger PWM
  // Endpoints: velocity 127 reaches max; velocity 1 reaches ~min.
  cfg.notes[0].airflowNominalPercent=40;
  ac.setAirflowForNote(60,127); uint16_t hi=lastOff;
  ac.setAirflowForNote(60,1);   uint16_t lo=lastOff;
  assert(hi>lo);
  // velocity response 0 -> velocity ignored, hold the nominal regardless of velocity.
  cfg.airVelocityResponse=0;
  cfg.notes[0].airflowNominalPercent=30; ac.setAirflowForNote(60,10); uint16_t n1=lastOff;
  ac.setAirflowForNote(60,120); uint16_t n2=lastOff;
  assert(n1==n2);   // same nominal-held angle for any velocity when vr=0
}

// --- §14: YIN pitch core on synthetic PCM -----------------------------------
static void fillTone(float* buf, int n, float f0, float amp, float dc,
                     float noise, bool twoHarmonics, bool clip){
  unsigned int seed = 12345u;
  for (int i = 0; i < n; i++){
    float t = (float)i / (float)MIC_SAMPLE_RATE;
    float v = amp * sinf(2.0f*(float)M_PI*f0*t);
    if (twoHarmonics) v += 0.35f*amp*sinf(2.0f*(float)M_PI*2.0f*f0*t);
    v += dc;
    if (noise > 0.0f){ seed = seed*1103515245u + 12345u; float r = ((seed>>16)&0x7fff)/32767.0f - 0.5f; v += noise*2.0f*r; }
    if (clip){ if (v > 0.5f) v = 0.5f; if (v < -0.5f) v = -0.5f; }
    buf[i] = v;
  }
}
static void audio_yin_pcm_core(){
  PitchDetector det;
  std::vector<float> buf(MIC_BUFFER_SIZE);
  // Pure sine, musical range: exact MIDI note, no octave error.
  float pure[] = {220.0f, 262.0f, 440.0f, 587.33f, 1046.5f};
  for (float f0 : pure){
    fillTone(buf.data(), MIC_BUFFER_SIZE, f0, 0.4f, 0.0f, 0.0f, false, false);
    PitchResult r = det.detect(buf.data(), MIC_BUFFER_SIZE);
    assert(r.valid);
    assert(PitchMath::hzToMidi(r.hz) == PitchMath::hzToMidi(f0));
    assert(r.confidence > 0.7f);
  }
  // Near the upper limit (200-4000 Hz range) the fixed 32 kHz tau resolution
  // limits precision: require detection within one semitone, no octave error.
  for (float f0 : {2500.0f, 3000.0f, 3800.0f}){
    fillTone(buf.data(), MIC_BUFFER_SIZE, f0, 0.4f, 0.0f, 0.0f, false, false);
    PitchResult r = det.detect(buf.data(), MIC_BUFFER_SIZE);
    assert(r.valid);
    int d = PitchMath::hzToMidi(r.hz) - PitchMath::hzToMidi(f0);
    assert(d >= -1 && d <= 1);
  }
  // Sine + DC offset: DC removal must keep detection correct.
  fillTone(buf.data(), MIC_BUFFER_SIZE, 440.0f, 0.4f, 0.3f, 0.0f, false, false);
  { PitchResult r = det.detect(buf.data(), MIC_BUFFER_SIZE); assert(r.valid && PitchMath::hzToMidi(r.hz)==69); }
  // Sine + moderate noise: still detects the fundamental.
  fillTone(buf.data(), MIC_BUFFER_SIZE, 440.0f, 0.4f, 0.0f, 0.05f, false, false);
  { PitchResult r = det.detect(buf.data(), MIC_BUFFER_SIZE); assert(r.valid && PitchMath::hzToMidi(r.hz)==69); }
  // Fundamental + strong harmonic: must return the fundamental (no octave-down).
  fillTone(buf.data(), MIC_BUFFER_SIZE, 523.25f, 0.4f, 0.0f, 0.0f, true, false);
  { PitchResult r = det.detect(buf.data(), MIC_BUFFER_SIZE); assert(r.valid && PitchMath::hzToMidi(r.hz)==72); }
  // Clipped/saturated sine: harmonics rich but fundamental still dominant.
  fillTone(buf.data(), MIC_BUFFER_SIZE, 440.0f, 0.9f, 0.0f, 0.0f, false, true);
  { PitchResult r = det.detect(buf.data(), MIC_BUFFER_SIZE); assert(PitchMath::hzToMidi(r.hz)==69); }
  // Broadband noise: no reliable pitch (rejected).
  fillTone(buf.data(), MIC_BUFFER_SIZE, 0.0f, 0.0f, 0.0f, 0.4f, false, false);
  { PitchResult r = det.detect(buf.data(), MIC_BUFFER_SIZE); assert(!r.valid); }
  // Silence: no pitch.
  for (int i=0;i<MIC_BUFFER_SIZE;i++) buf[i]=0.0f;
  { PitchResult r = det.detect(buf.data(), MIC_BUFFER_SIZE); assert(!r.valid && r.hz==0.0f); }
  // RMS: silence ~0, tone > 0.
  for (int i=0;i<MIC_BUFFER_SIZE;i++) buf[i]=0.0f;
  assert(PitchDetector::rms(buf.data(), MIC_BUFFER_SIZE) < 1e-4f);
  fillTone(buf.data(), MIC_BUFFER_SIZE, 440.0f, 0.4f, 0.2f, 0.0f, false, false);
  assert(PitchDetector::rms(buf.data(), MIC_BUFFER_SIZE) > 0.2f);   // DC-removed, ~amp/sqrt2
}
// --- §15: raw microphone-signal classification ------------------------------
static void audio_mic_classification(){
  const int N = 256;
  std::vector<int32_t> raw(N);
  // all-zero -> not connected
  for (int i=0;i<N;i++) raw[i]=0;
  assert(PitchDetector::classifyRaw(raw.data(), N) == MIC_SIG_ALL_ZERO);
  // stuck constant non-zero -> stuck line
  for (int i=0;i<N;i++) raw[i] = (int32_t)1000 << 8;
  assert(PitchDetector::classifyRaw(raw.data(), N) == MIC_SIG_STUCK);
  // permanently saturated
  for (int i=0;i<N;i++) raw[i] = (int32_t)0x7FFFFF << 8;
  assert(PitchDetector::classifyRaw(raw.data(), N) == MIC_SIG_SATURATED);
  // varied signal -> ok
  for (int i=0;i<N;i++){ int32_t v = (int32_t)(4000000.0 * sin(2.0*M_PI*i/32.0)); raw[i] = v << 8; }
  assert(PitchDetector::classifyRaw(raw.data(), N) == MIC_SIG_OK);
}

int main(){ pca_detection_safe_boot(); reservoir_autostart_behaviour(); cc73_does_not_mutate_persistent_cfg(); pressure_direct_pwm_once(); pressure_hall_pid_once_and_guards(); event_queue_cases(); note_sequencer_min_and_panic(); note_sequencer_monophonic_replacement(); fan_autonomous(); midi_validation_edges(); air_modes_paths(); autocal_pitch_conversions(); autocal_math_helpers(); autocal_config_nominal_validation(); autocal_integration_minmax_nominal(); autocal_keep_old_on_fail(); autocal_timeout_safe_stop(); autocal_mic_absent(); airflow_nominal_drives_angle(); autocal_frozen_source_fails(); autocal_air_supply_gate(); autocal_14_notes_no_timeout(); autocal_plus70_cents_rejected(); autocal_storage_failure_restores(); autocal_range_finder(); autocal_range_finder_stale(); autocal_range_apply_storage(); autocal_air_lost_midnote(); calair_reservoir_requires_sensor(); instrument_power_held_during_actuator_session(); instrument_ignores_midi_during_calibration(); instrument_inert_after_pca_failure(); autocal_global_timeout_scales_to_max_notes(); airflow_cc2_silence_and_live_cc(); airflow_attack_cancelled_on_rest(); airflow_cc2_timeout_on_held_note(); audio_yin_pcm_core(); audio_mic_classification(); std::cout << "behavior tests passed\n"; }

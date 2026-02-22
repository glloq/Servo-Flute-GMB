#ifndef AIRFLOW_CONTROLLER_H
#define AIRFLOW_CONTROLLER_H

#include <Arduino.h>
#include <Adafruit_PWMServoDriver.h>
#include "settings.h"

class AirflowController {
public:
  AirflowController(Adafruit_PWMServoDriver& pwm);

  void begin();

  // Definit le debit d'air selon la velocite MIDI (1-127)
  void setAirflowVelocity(byte velocity);

  // Definit le debit d'air pour une note specifique avec velocite
  void setAirflowForNote(byte midiNote, byte velocity);

  void openValve();
  void closeValve();
  bool isValveOpen() const;
  uint16_t getAirflowAngle() const { return _baseAngleWithoutVibrato; }

  // Deprecated aliases (retro-compat)
  void openSolenoid() { openValve(); }
  void closeSolenoid() { closeValve(); }
  bool isSolenoidOpen() const { return isValveOpen(); }
  void setAirflowToRest();

  // Methode update pour gestion PWM solenoide et vibrato (appeler dans loop)
  void update();

  // Met a jour les valeurs CC (appele par InstrumentManager)
  void setCCValues(byte ccVolume, byte ccExpression, byte ccModulation);

  // CC73 (Attack Time) : change le mode d'attaque en temps reel
  // 0-42=stable, 43-84=accent, 85-127=crescendo ; valeur dans la plage = intensite
  void setCC73Attack(byte ccValue);

  // Met a jour CC2 (Breath Controller) avec lissage et fallback velocity
  void updateCC2Breath(byte ccBreath);

  // Ajustement live du souffle depuis le slider UI (pourcentage 0-100)
  void setAirflowLivePercent(uint8_t percent);

  // Calibration : positionner le servo airflow a un angle arbitraire
  void testAirflowAngle(uint16_t angle);
  // Calibration : tester solenoide open/close
  void testSolenoid(bool open);

private:
  Adafruit_PWMServoDriver& _pwm;
  bool _solenoidOpen;
  unsigned long _solenoidOpenTime;

  // Valeurs Control Change MIDI
  byte _ccVolume;
  byte _ccExpression;
  byte _ccModulation;
  byte _ccBreath;

  // Breath Controller (CC2)
  byte _cc2SmoothingBuffer[CC2_SMOOTHING_BUFFER_SIZE];
  uint8_t _cc2BufferIndex;
  uint8_t _cc2BufferCount;
  unsigned long _lastCC2Time;
  byte _lastVelocity;

  // Gestion vibrato
  uint16_t _baseAngleWithoutVibrato;
  bool _vibratoActive;
  uint16_t _currentMinAngle;
  uint16_t _currentMaxAngle;

  // Transition attaque (modes accent/crescendo)
  bool _attackActive;
  unsigned long _attackStartTime;
  uint16_t _attackStartAngle;
  uint16_t _attackTargetAngle;

  void setAirflowServoAngle(uint16_t angle);
  uint16_t angleToPWM(uint16_t angle);
  void setSolenoidPWM(uint8_t pwmValue);
  void setValveServoAngle(bool open);
};

#endif

#ifndef FAN_CONTROLLER_H
#define FAN_CONTROLLER_H

#include <Arduino.h>
#include "settings.h"

class FanController {
public:
  FanController();

  // Initialise le GPIO PWM du ventilateur
  void begin();

  // Definir la vitesse (0-100%)
  void setSpeed(uint8_t percent);

  // Arreter immediatement
  void stop();

  // Signaler note On/Off pour gestion idle
  void onNoteOn();
  void onNoteOff();

  // Appeler dans loop() - gere la rampe de demarrage + idle timeout
  void update();

  // Accesseurs
  uint8_t getSpeed() const { return _speedPercent; }
  uint8_t getPwm() const { return _currentPwm; }
  bool isRunning() const { return _currentPwm > 0; }
  bool isReady() const { return _ready; }
  bool isIdle() const { return _idleActive; }

private:
  uint8_t _speedPercent;    // Vitesse cible (0-100)
  uint8_t _currentPwm;     // PWM actuel
  uint8_t _targetPwm;      // PWM cible
  bool _ready;              // Ventilateur a atteint la vitesse cible
  unsigned long _rampStartTime;
  uint8_t _rampStartPwm;

  // Idle management
  bool _idleActive;               // En mode idle (entre notes)
  unsigned long _lastNoteOffTime; // Timestamp du dernier noteOff
  bool _notePlaying;              // Une note est en cours

  uint8_t percentToPwm(uint8_t percent);
};

#endif

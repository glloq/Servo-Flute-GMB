/***********************************************************************************************
 * StatusLed - Gestion de la LED d'etat avec patterns non-bloquants
 *
 * Patterns supportes :
 * - OFF            : LED eteinte
 * - ON             : LED allumee fixe
 * - BLINK_FAST     : Clignotement rapide (advertising BLE / connexion WiFi)
 * - BLINK_SLOW     : Clignotement lent (connecte, attente MIDI)
 * - DOUBLE_FLASH   : Double flash (WiFi STA connecte)
 * - TRIPLE_FLASH   : Triple flash (mode hotspot actif)
 ***********************************************************************************************/
#ifndef STATUS_LED_H
#define STATUS_LED_H

#include <Arduino.h>
#include "settings.h"

enum LedPattern {
  LED_OFF,
  LED_ON,
  LED_BLINK_FAST,
  LED_BLINK_SLOW,
  LED_DOUBLE_FLASH,
  LED_TRIPLE_FLASH
};

class StatusLed {
public:
  StatusLed(uint8_t pin);

  void begin();
  void update();
  void setPattern(LedPattern pattern);
  LedPattern getPattern() const;

private:
  uint8_t _pin;
  LedPattern _pattern;
  bool _ledState;
  unsigned long _lastToggle;
  uint8_t _flashCount;       // Compteur de flashs dans un cycle
  bool _inPause;             // En pause entre les groupes de flashs
};

#endif

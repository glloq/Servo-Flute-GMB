/***********************************************************************************************
 * BleMidiHandler - Gestion BLE-MIDI via NimBLE
 *
 * Utilise la bibliotheque Arduino-BLE-MIDI (lathoub) avec backend NimBLE
 * pour une consommation memoire reduite (~200KB flash en moins vs Bluedroid).
 *
 * Fonctionnalites :
 * - Advertising BLE-MIDI automatique
 * - Reception Note On/Off et Control Change
 * - Gestion connexion/deconnexion
 * - Filtrage canal MIDI
 *
 * Dependances :
 * - h2zero/NimBLE-Arduino
 * - lathoub/BLE-MIDI (avec BLEMIDI_ESP32_NimBLE.h)
 ***********************************************************************************************/
#ifndef BLE_MIDI_HANDLER_H
#define BLE_MIDI_HANDLER_H

#include <Arduino.h>
#include "settings.h"

// Forward declaration
class InstrumentManager;

class BleMidiHandler {
public:
  BleMidiHandler();

  // Initialise le BLE-MIDI et demarre l'advertising
  void begin(InstrumentManager* instrument);

  // Met a jour le BLE-MIDI (appeler dans loop)
  void update();

  // Demarre/arrete l'advertising BLE
  void startAdvertising();
  void stopAdvertising();

  // Etat de la connexion
  bool isConnected() const;
  bool isAdvertising() const;

private:
  InstrumentManager* _instrument;
  bool _connected;
  bool _advertising;

  // Callbacks BLE-MIDI (fonctions statiques pour les callbacks)
  static BleMidiHandler* _instance;
  static void onNoteOn(byte channel, byte note, byte velocity);
  static void onNoteOff(byte channel, byte note, byte velocity);
  static void onControlChange(byte channel, byte number, byte value);
  static void onConnected();
  static void onDisconnected();

  bool isChannelAccepted(byte channel);
};

#endif

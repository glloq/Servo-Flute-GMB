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
 * - SysEx bidirectionnel : sert la reconnaissance General-Midi-Boop (blocs 1 /
 *   0x10 / 0x11) via GmbMidiBridge. Le protocole lui-meme n'est PAS duplique
 *   ici : ce handler n'est qu'un port (IGmbMidiPort).
 *
 * Dependances :
 * - h2zero/NimBLE-Arduino
 * - lathoub/BLE-MIDI (avec BLEMIDI_ESP32_NimBLE.h)
 ***********************************************************************************************/
#ifndef BLE_MIDI_HANDLER_H
#define BLE_MIDI_HANDLER_H

#include <Arduino.h>
#include "settings.h"
#include "gmb/GmbMidiPort.h"

// Forward declaration
class InstrumentManager;

class BleMidiHandler : public gmb::IGmbMidiPort {
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

  // --- IGmbMidiPort : BLE-MIDI est bidirectionnel ---
  bool canSendSysEx() const override { return _connected; }
  void sendSysEx(const uint8_t* data, size_t len) override;
  const char* gmbPortName() const override { return "ble"; }

private:
  InstrumentManager* _instrument;
  bool _connected;
  bool _advertising;

  // Callbacks BLE-MIDI (fonctions statiques pour les callbacks)
  static BleMidiHandler* _instance;
  static void onNoteOn(byte channel, byte note, byte velocity);
  static void onNoteOff(byte channel, byte note, byte velocity);
  static void onControlChange(byte channel, byte number, byte value);
  static void onSystemExclusive(byte* data, unsigned size);
  static void onConnected();
  static void onDisconnected();

  bool isChannelAccepted(byte channel);
};

#endif

/***********************************************************************************************
 * WirelessManager - Orchestrateur des modes sans fil
 *
 * Gere la logique de haut niveau pour les 3 modes de fonctionnement :
 *
 * 1. MODE BLUETOOTH : BLE-MIDI via NimBLE
 *    - Bouton court : restart advertising
 *    - LED : cligno rapide (advertising) / lent (connecte)
 *
 * 2. MODE WIFI STA : rtpMIDI + serveur web sur reseau existant
 *    - Connexion automatique au WiFi sauvegarde
 *    - Fallback AP si connexion echoue
 *    - Bouton long : force AP
 *    - LED : double flash (STA connecte)
 *
 * 3. MODE WIFI AP (Hotspot) : rtpMIDI + serveur web en hotspot
 *    - Fallback automatique si pas de WiFi configure
 *    - Force par bouton long en mode WiFi
 *    - LED : triple flash (AP actif)
 *
 * En mode WiFi (STA et AP) :
 * - Serveur web avec clavier virtuel, lecteur MIDI, config, monitoring
 * - WebSocket pour controle temps reel
 * - Upload et lecture de fichiers MIDI
 *
 * Coordonne : BleMidiHandler, WifiMidiHandler, WebConfigurator,
 *             MidiFilePlayer, StatusLed, HardwareInputs
 ***********************************************************************************************/
#ifndef WIRELESS_MANAGER_H
#define WIRELESS_MANAGER_H

#include <Arduino.h>
#include "settings.h"
#include "StatusLed.h"
#include "HardwareInputs.h"
#include "BleMidiHandler.h"
#include "WifiMidiHandler.h"
#include "WebConfigurator.h"
#include "MidiFilePlayer.h"

// Forward declaration
class InstrumentManager;

class WirelessManager {
public:
  WirelessManager(StatusLed& led, HardwareInputs& inputs);

  // Initialise le mode sans fil selon le switch
  void begin(InstrumentManager* instrument);

  // Met a jour (appeler dans loop)
  void update();

  // Etat courant
  OperatingMode getMode() const;
  bool isMidiConnected() const;
  String getStatusText() const;

  // Acces au handler WiFi (pour scan, connect depuis le web)
  WifiMidiHandler& getWifiMidi() { return _wifiMidi; }

private:
  StatusLed& _led;
  HardwareInputs& _inputs;
  InstrumentManager* _instrument;

  OperatingMode _currentMode;

  BleMidiHandler _bleMidi;
  WifiMidiHandler _wifiMidi;
  WebConfigurator* _webConfig;
  MidiFilePlayer* _midiPlayer;

  // Gestion des evenements bouton
  void handleButtonEvent(ButtonEvent event);

  // Met a jour le pattern LED selon l'etat
  void updateLedPattern();
};

#endif

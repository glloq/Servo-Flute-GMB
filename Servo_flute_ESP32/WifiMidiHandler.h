/***********************************************************************************************
 * WifiMidiHandler - Gestion WiFi MIDI via rtpMIDI (AppleMIDI)
 *
 * Utilise la bibliotheque Arduino-AppleMIDI-Library (lathoub) pour
 * la compatibilite avec :
 * - macOS MIDI Network Setup (natif)
 * - Windows rtpMIDI (Tobias Erichsen)
 * - Linux (via avahi)
 *
 * Gere aussi le serveur web de configuration et mDNS.
 *
 * Modes WiFi :
 * - STA : connexion a un reseau existant
 * - AP  : hotspot autonome (fallback ou force par bouton)
 *
 * Dependances :
 * - lathoub/AppleMIDI
 * - FortySevenEffects/MIDI Library
 * - ESPmDNS (built-in)
 ***********************************************************************************************/
#ifndef WIFI_MIDI_HANDLER_H
#define WIFI_MIDI_HANDLER_H

#include <Arduino.h>
#include <WiFi.h>
#include "settings.h"

// Forward declaration
class InstrumentManager;

enum WifiState {
  WIFI_STATE_DISCONNECTED,
  WIFI_STATE_CONNECTING,
  WIFI_STATE_STA_CONNECTED,
  WIFI_STATE_AP_ACTIVE
};

class WifiMidiHandler {
public:
  WifiMidiHandler();

  // Initialise le WiFi et rtpMIDI
  void begin(InstrumentManager* instrument);

  // Met a jour (appeler dans loop)
  void update();

  // Demarre en mode STA (connexion reseau)
  void startSTA(const char* ssid, const char* password);

  // Demarre en mode AP (hotspot)
  void startAP();

  // Force le passage en mode AP
  void forceAP();

  // Etat
  WifiState getState() const;
  bool isConnected() const;
  bool isAPMode() const;
  String getIPAddress() const;

  // WiFi scan
  void startWifiScan();
  String getScanResultsJson() const;
  bool isScanComplete() const;

  // Connexion a un nouveau reseau
  void connectToNetwork(const char* ssid, const char* password);

private:
  InstrumentManager* _instrument;
  WifiState _state;
  unsigned long _connectStartTime;

  // Callbacks MIDI (fonctions statiques)
  static WifiMidiHandler* _instance;
  static void onNoteOn(byte channel, byte note, byte velocity);
  static void onNoteOff(byte channel, byte note, byte velocity);
  static void onControlChange(byte channel, byte number, byte value);
  static void onAppleMidiConnected(const char* name);
  static void onAppleMidiDisconnected();

  bool isChannelAccepted(byte channel);
  void setupMDNS();
  void setupRtpMidi();
};

#endif

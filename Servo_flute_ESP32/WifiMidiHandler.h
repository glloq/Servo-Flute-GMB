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
 * SysEx bidirectionnel : sert la reconnaissance General-Midi-Boop (blocs 1 /
 * 0x10 / 0x11) via GmbMidiBridge, sans dupliquer la logique du protocole.
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
#include <DNSServer.h>
#include "settings.h"
#include "gmb/GmbMidiPort.h"

// Forward declaration
class InstrumentManager;

enum WifiState {
  WIFI_STATE_DISCONNECTED,
  WIFI_STATE_CONNECTING,
  WIFI_STATE_STA_CONNECTED,
  WIFI_STATE_AP_ACTIVE
};

class WifiMidiHandler : public gmb::IGmbMidiPort {
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

  // --- IGmbMidiPort : rtpMIDI est bidirectionnel une fois la session ouverte ---
  // Sans participant, AppleMIDI::beginTransmission() refuse l'envoi : on ne
  // pretend donc pas pouvoir repondre tant qu'aucune session n'est etablie.
  bool canSendSysEx() const override { return _sessionActive; }
  void sendSysEx(const uint8_t* data, size_t len) override;
  const char* gmbPortName() const override { return "rtpmidi"; }

private:
  InstrumentManager* _instrument;
  WifiState _state;
  unsigned long _connectStartTime;
  bool _sessionActive;   // au moins un participant rtpMIDI connecte

  // Callbacks MIDI (fonctions statiques)
  static WifiMidiHandler* _instance;
  static void onNoteOn(byte channel, byte note, byte velocity);
  static void onNoteOff(byte channel, byte note, byte velocity);
  static void onControlChange(byte channel, byte number, byte value);
  static void onSystemExclusive(byte* data, unsigned size);
  static void onAppleMidiConnected(const char* name);
  static void onAppleMidiDisconnected();

  bool isChannelAccepted(byte channel);
  void setupMDNS();
  void setupRtpMidi();

  // --- Transitions reseau centralisees (§10) --------------------------------
  // Chaque bascule AP <-> STA passe par ces deux points. Avant tout demontage,
  // handleTransportLost() est appele si une session rtpMIDI a pu etre active :
  // sans cela, une note tenue au moment d'un forceAP() n'aurait jamais recu son
  // Note Off et laissait la valve/le souffle/la pompe en marche.
  // Les demarrages sont idempotents : mDNS, AppleMIDI et le DNS captif ne sont
  // jamais reinitialises deux fois sans avoir ete arretes entre-temps.
  void stopNetworkServices(bool notifyTransportLost);
  void startNetworkServices();

  bool _mdnsStarted;
  bool _rtpMidiStarted;
  bool _captiveDnsStarted;

  // Captive portal DNS (mode AP uniquement)
  DNSServer _dnsServer;
  void startCaptiveDNS();
  void stopCaptiveDNS();
};

#endif

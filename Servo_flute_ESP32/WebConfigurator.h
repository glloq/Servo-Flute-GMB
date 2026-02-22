/***********************************************************************************************
 * WebConfigurator - Serveur web async avec interface de controle complete
 *
 * Fonctionnalites :
 * - Page web : clavier virtuel, lecteur MIDI, config, monitoring
 * - WebSocket : controle temps reel (clavier virtuel, feedback status)
 * - API REST : upload fichier MIDI, lecture/modification config
 * - HTML/CSS/JS embarque en PROGMEM (pas de fichier externe necessaire)
 *
 * Endpoints :
 * - GET  /              Page principale (SPA avec onglets)
 * - GET  /api/status    Etat JSON (mode, IP, notes, CC, player)
 * - GET  /api/config    Configuration JSON
 * - POST /api/config    Modifier la configuration
 * - POST /api/midi      Upload fichier MIDI
 * - GET  /api/wifi/scan       Lancer scan WiFi
 * - GET  /api/wifi/results    Resultats scan WiFi
 * - POST /api/wifi/connect    Connexion a un reseau WiFi
 * - WS   /ws            WebSocket pour controle temps reel
 *
 * Protocole WebSocket (JSON) :
 * Client→Serveur :
 *   {"t":"non","n":82,"v":100}  Note On
 *   {"t":"nof","n":82}          Note Off
 *   {"t":"cc","c":7,"v":100}    Control Change
 *   {"t":"play"}                Play fichier MIDI
 *   {"t":"pause"}               Pause
 *   {"t":"stop"}                Stop
 *   {"t":"velocity","v":100}    Changer velocity par defaut
 *   {"t":"test_finger","i":0,"a":90}  Test servo doigt
 *   {"t":"test_air","a":60}           Test servo airflow
 *   {"t":"test_sol","o":true}         Test solenoide
 *
 * Serveur→Client :
 *   {"t":"status",...}          Mise a jour status periodique
 *   {"t":"midi_loaded",...}     Fichier MIDI charge avec succes
 *   {"t":"midi_error","msg":""} Erreur chargement MIDI
 *
 * Dependances : ESPAsyncWebServer, AsyncTCP, LittleFS, ArduinoJson
 ***********************************************************************************************/
#ifndef WEB_CONFIGURATOR_H
#define WEB_CONFIGURATOR_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "settings.h"
#include "ConfigStorage.h"
#include "MidiFilePlayer.h"

#if MIC_ENABLED
#include "AudioAnalyzer.h"
#include "AutoCalibrator.h"
#endif

// Forward declarations
class InstrumentManager;
class WirelessManager;

class WebConfigurator {
public:
  WebConfigurator(uint16_t port = WEB_SERVER_PORT);
  ~WebConfigurator();

  void begin(InstrumentManager* instrument, MidiFilePlayer* player);
  void update();

  // Definir le WirelessManager pour acceder aux infos de status
  void setWirelessManager(WirelessManager* wm);

private:
  AsyncWebServer _server;
  AsyncWebSocket _ws;
  InstrumentManager* _instrument;
  MidiFilePlayer* _player;
  WirelessManager* _wirelessManager;

  // Velocity par defaut pour le clavier virtuel
  uint8_t _webVelocity;

  // Timing status broadcast
  unsigned long _lastStatusBroadcast;
  unsigned long _lastWsCleanup;

  // Setup des routes HTTP
  void setupRoutes();

  // Handlers HTTP
  void handleRoot(AsyncWebServerRequest* request);
  void handleApiStatus(AsyncWebServerRequest* request);
  void handleApiConfig(AsyncWebServerRequest* request);
  void handleApiConfigFinalize(AsyncWebServerRequest* request);
  void handleApiConfigReset(AsyncWebServerRequest* request);
  void handleApiFactoryReset(AsyncWebServerRequest* request);
  void handleMidiUpload(AsyncWebServerRequest* request, const String& filename,
                        size_t index, uint8_t* data, size_t len, bool final);
  void handleMidiUploadComplete(AsyncWebServerRequest* request);
  void handleMidiList(AsyncWebServerRequest* request);
  void handleMidiDelete(AsyncWebServerRequest* request);
  void handleMidiLoad(AsyncWebServerRequest* request);

  // Calcule la taille totale des fichiers dans MIDI_DIR (octets)
  size_t getMidiStorageUsed();

  // Handler WebSocket
  void onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                 AwsEventType type, void* arg, uint8_t* data, size_t len);
  void processWsMessage(AsyncWebSocketClient* client, uint8_t* data, size_t len);

  // Broadcast status a tous les clients WS
  void broadcastStatus();

  // Buffer body pour POST config
  String _configBody;

  // Fichier temporaire pour upload MIDI
  File _uploadFile;
  size_t _uploadSize;
  String _uploadFileName;  // Nom original du fichier uploade

#if MIC_ENABLED
  // Audio analyzer (INMP441 microphone)
  AudioAnalyzer* _audio;
  AutoCalibrator* _autoCal;
  bool _micMonitorEnabled;
  unsigned long _lastAudioBroadcast;
  unsigned long _lastAcalBroadcast;
#endif
};

#endif

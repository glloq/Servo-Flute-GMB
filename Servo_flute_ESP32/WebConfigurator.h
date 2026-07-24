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
  void handleApiDiagnostics(AsyncWebServerRequest* request);
  void handleMidiUpload(AsyncWebServerRequest* request, const String& filename,
                        size_t index, uint8_t* data, size_t len, bool final);
  void handleMidiUploadComplete(AsyncWebServerRequest* request);
  void handleMidiList(AsyncWebServerRequest* request);
  void handleMidiDelete(AsyncWebServerRequest* request);
  void handleMidiLoad(AsyncWebServerRequest* request);
  void handleApiWifiConnect(AsyncWebServerRequest* request);

  // Calcule la taille totale des fichiers dans MIDI_DIR (octets)
  size_t getMidiStorageUsed();

  // Handler WebSocket
  void onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                 AwsEventType type, void* arg, uint8_t* data, size_t len);
  void processWsMessage(AsyncWebSocketClient* client, uint8_t* data, size_t len);

  // Broadcast status a tous les clients WS
  void broadcastStatus();

  // Small JSON POST bodies are accumulated per-request in AsyncWebServerRequest::
  // _tempObject (see WebReqBody in the .cpp) so concurrent requests never share a
  // buffer; there is no shared body member.

  // Manual actuator test session (owner + server-side timeout). A manual test
  // (valve/pump/fan/servo) is owned by the WS client that issued it and is bounded
  // by TEST_SESSION_MAX_MS; only the owner's disconnect returns hardware to safe.
  uint32_t _testOwnerClientId = 0;
  unsigned long _testStartTime = 0;
  bool _testActive = false;
  // Start/refresh the manual-test window. Returns false (and changes nothing) if a
  // test is already active and owned by a different client, so ownership can't be
  // stolen mid-test.
  bool beginTestSession(uint32_t clientId);
  void endTestSession(bool safeHardware);     // stop it (optionally safing hardware)

  // Controlled restart: after a restart-required config change / reset, safe the
  // hardware and schedule a reboot so the change takes effect cleanly. While set,
  // config-mutating routes are refused so they cannot overwrite the pending config.
  unsigned long _pendingRestartTime = 0;
  void scheduleControlledRestart();
  bool restartPending() const { return _pendingRestartTime != 0; }

  // Fichier temporaire pour upload MIDI
  File _uploadFile;
  size_t _uploadSize;
  String _uploadFileName;  // Nom original du fichier uploade
  bool _uploadError;       // Erreur pendant l'upload (ex: echec ouverture fichier temp)

#if MIC_ENABLED
  // Audio analyzer (INMP441 microphone)
  AudioAnalyzer* _audio;
  AutoCalibrator* _autoCal;
  bool _micMonitorEnabled;
  unsigned long _lastAudioBroadcast;
  unsigned long _lastAcalBroadcast;
  // Actuator-session ownership: the WS client that started the running
  // calibration owns it (only it may stop/apply; its disconnect stops it).
  uint32_t _autoCalOwnerClientId;
  // User's mic-monitor preference captured when a calibration starts, restored
  // when it ends so calibration never permanently changes the monitor choice.
  bool _micMonitorBeforeCalibration;
  // The range-finder result is broadcast once; the calibrator then stays in
  // ACAL_RF_COMPLETE (kept applicable) until apply / cancel / new start / owner
  // disconnect, so this guards against re-broadcasting rf_done every loop.
  bool _rfDoneSent;
  unsigned long _rfDoneTime;   // when rf_done was sent, for the review-window timeout

  bool isCalibrationActive() const;
  // Guard for every config-mutating REST route: if a calibration is active it
  // sends HTTP 409 {"ok":false,"error":"calibration_active"} and returns true.
  bool rejectIfCalibrationActive(AsyncWebServerRequest* request);
  // Stop any running calibration and return the microphone monitor to the user's
  // pre-calibration state. Safe to call when nothing is running.
  void cancelActiveActuatorSession();
  // True (and sends a calibration_active error) if `type` is an actuator command
  // that must be blocked while a calibration owns the actuators.
  bool actuatorCommandBlockedDuringCalibration(AsyncWebSocketClient* client, const char* type);
#endif
};

#endif

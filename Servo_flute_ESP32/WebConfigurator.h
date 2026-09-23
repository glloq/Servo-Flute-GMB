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
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include "settings.h"
#include "ConfigStorage.h"
#include "MidiFilePlayer.h"
#include "WebAuth.h"

#if MIC_ENABLED
#include "AudioAnalyzer.h"
#include "AutoCalibrator.h"
#endif

// Forward declarations
class InstrumentManager;
class WirelessManager;

// Operations web qui touchent la configuration active, LittleFS, le lecteur MIDI
// ou le calibrateur. Elles ne s'executent JAMAIS depuis la tache AsyncTCP : le
// callback remplit une operation, la poste, puis attend (borne) que loop()
// l'execute. La tache loop() reste ainsi l'unique proprietaire de `cfg`, des
// actionneurs et du systeme de fichiers.
enum WebOpType : uint8_t {
  WEBOP_NONE = 0,
  WEBOP_COMMIT_CONFIG,
  WEBOP_RESET_CONFIG,
  WEBOP_FACTORY_RESET,
  WEBOP_RESTART,
  WEBOP_FORMAT_FS,
  WEBOP_WIFI_CONNECT,
  WEBOP_MIDI_DELETE,
  WEBOP_MIDI_LOAD,
  WEBOP_MIDI_FINALIZE,
  WEBOP_PLAYER_PLAY,
  WEBOP_PLAYER_PAUSE,
  WEBOP_PLAYER_STOP,
  WEBOP_PLAYER_CH_FILTER,
  WEBOP_AUTOCAL_START_AIR,
  WEBOP_AUTOCAL_START_RANGE,
  WEBOP_AUTOCAL_APPLY_RANGE,
  WEBOP_MIC_MONITOR,
  WEBOP_MIC_RESET,
  WEBOP_NOISE_START,
  WEBOP_NOISE_STOP,
  WEBOP_NOISE_RESET,
  WEBOP_SET_ADMIN_PASSWORD,
  WEBOP_REGEN_AP_PASSWORD
};

struct WebOp {
  WebOpType type = WEBOP_NONE;
  uint32_t seq = 0;                     // identifie CETTE operation (anti-reliquat)
  // Entrees
  RuntimeConfig* candidate = nullptr;   // WEBOP_COMMIT_CONFIG (alloue par l'appelant)
  String strA;                          // nom de fichier / SSID / mot de passe
  String strB;                          // mot de passe Wi-Fi
  uint32_t clientId = 0;                // client WebSocket a l'origine
  int32_t intA = 0;
  // Sorties
  bool ok = false;
  int httpStatus = 200;
  String json;                          // corps JSON complet de la reponse
};

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

  // --- Hand-off AsyncTCP -> loop() -----------------------------------------
  // Un seul emplacement, serialise par _opMutex. Le producteur (tache AsyncTCP)
  // attend au plus WEBOP_TIMEOUT_MS que loop() execute l'operation, puis lit le
  // resultat. La boucle principale n'attend jamais AsyncTCP : pas d'interblocage.
  WebOp _op;
  volatile bool _opPending;
  volatile bool _opAbandoned;           // l'appelant a renonce : ne pas appliquer
  volatile uint32_t _opDoneSeq;         // sequence de la derniere operation terminee
  uint32_t _opSeqCounter;
  SemaphoreHandle_t _opMutex;
  SemaphoreHandle_t _opDone;
  // Hand-off BLOQUANT, reserve aux handlers HTTP. A ne JAMAIS appeler depuis un
  // callback WebSocket : celui-ci peut detenir le verrou interne d'AsyncWebSocket,
  // que loop() prend a son tour pour diffuser un statut - l'attente croisee
  // bloquerait les deux taches.
  bool runOnLoop(WebOp& op);
  void servicePendingOp();

  // Hand-off NON bloquant, utilise par les commandes WebSocket. Le resultat
  // eventuel est diffuse par loop() sur le WebSocket.
  static const uint8_t kWsOpQueueSize = 6;
  WebOp _wsOps[kWsOpQueueSize];
  uint8_t _wsOpHead;
  uint8_t _wsOpTail;
  uint8_t _wsOpCount;
  // Un MUTEX, pas un portMUX : une WebOp porte des String, donc la copier alloue
  // sur le tas. Faire cela dans une section critique (interruptions coupees,
  // spinlock pris) est interdit - l'allocateur prend lui-meme un verrou. Un
  // mutex FreeRTOS autorise l'allocation et la contention est negligeable
  // (AsyncTCP produit, loop() consomme, le temps de garde est de l'ordre de la
  // microseconde).
  SemaphoreHandle_t _wsOpMutex;
  bool postWebOp(const WebOp& op);
  void serviceWsOps();

  // Annulation de calibration NON perdable. postWebOp() peut echouer quand la
  // file est pleine, et plusieurs appelants ignoraient ce resultat : le panic
  // coupait alors les actionneurs mais _autoCal restait "running" et reprenait
  // au cycle suivant en reappliquant ses commandes. Meme principe que le panic :
  // un drapeau dedie, consomme au debut de update().
  volatile bool _calCancelRequested;
  void requestCalibrationCancel() { _calCancelRequested = true; }
  void executeWebOp(WebOp& op);
  // Libere les ressources portees par l'operation (candidat de configuration),
  // qu'elle ait ete appliquee ou abandonnee. Idempotent.
  static void releaseWebOp(WebOp& op);

  // --- Authentification ----------------------------------------------------
  WebAuth _auth;
  // Clients WebSocket authentifies. On memorise le JETON, pas seulement
  // l'identifiant de client : sinon une socket ouverte restait authentifiee bien
  // au-dela du TTL de session, puisque plus rien ne reinterrogeait WebAuth.
  // Chaque commande revalide le jeton, ce qui applique l'expiration ET fait
  // glisser la fenetre comme pour HTTP.
  struct WsSession {
    uint32_t clientId;
    char token[WEB_AUTH_TOKEN_LEN + 1];
  };
  WsSession _wsSessions[WS_MAX_CLIENTS];
  bool isWsAuthenticated(uint32_t clientId);
  void setWsAuthenticated(uint32_t clientId, const String& token);
  void clearWsAuthentication(uint32_t clientId);

  // Copie coherente de la configuration active pour les lecteurs AsyncTCP.
  // `cfg = candidate` n'est pas atomique vis-a-vis d'un autre coeur : un
  // constructeur de reponse HTTP qui parcourt cfg pendant le commit pourrait
  // voir un struct a moitie recopie. Le commit et les gros lecteurs prennent
  // donc ce mutex, tenu le temps d'une copie de structure.
  SemaphoreHandle_t _cfgMutex;
  // Vrai si la CREATION du mutex a echoue (tas epuise), par opposition au
  // simple fait qu'il n'existe pas encore avant begin(). Sans cette
  // distinction, lockConfig() confondait "rien a serialiser" et "plus rien
  // pour serialiser" - voir lockConfig().
  bool _cfgMutexFailed = false;
  bool lockConfig(uint32_t timeoutMs = WEB_CONFIG_LOCK_MS);
  void unlockConfig();
  // Adaptateurs passes a commitCandidateConfig() : le verrou n'entoure que
  // l'affectation atomique `cfg = candidat`, pas la validation ni la flash.
  static bool cfgGuardLock(void* ctx);
  static void cfgGuardUnlock(void* ctx);
  // Extrait le jeton d'une requete (en-tete X-Auth-Token ou parametre ?token=).
  String extractToken(AsyncWebServerRequest* request) const;
  // Renvoie true (et repond 401) si la requete n'est pas authentifiee.
  bool rejectIfUnauthorized(AsyncWebServerRequest* request);
  void handleApiLogin(AsyncWebServerRequest* request);
  void handleApiAuthStatus(AsyncWebServerRequest* request);
  void handleApiAuthPassword(AsyncWebServerRequest* request);

  // --- Protection hardware_not_ready ---------------------------------------
  // Vrai si l'instrument existe ET a termine son initialisation hardware.
  bool hardwareReady() const;
  // Repond 503 {"ok":false,"error":"hardware_not_ready"} et renvoie true.
  bool rejectIfHardwareNotReady(AsyncWebServerRequest* request);
  // Commande WebSocket qui met physiquement un actionneur en mouvement.
  static bool isPhysicalWsCommand(const char* type);

  // --- Limiteur de debit des commandes d'actionneur ------------------------
  // Les CC MIDI avaient deja le leur (CC_RATE_LIMIT_PER_SECOND) ; le WebSocket
  // n'en avait aucun. Fenetre glissante par SERVEUR, pas par client : ce qu'on
  // protege est partage (file de commandes, bus I2C), et une session de test
  // manuelle n'a de toute facon qu'un seul proprietaire. Touche uniquement
  // depuis la tache AsyncTCP (tous les callbacks WebSocket y tournent), donc
  // sans verrou.
  unsigned long _wsActuatorWindowStart = 0;
  uint16_t _wsActuatorCount = 0;
  bool allowActuatorCommand(unsigned long now);

  // --- Observation des paniques -------------------------------------------
  // Derniere valeur de InstrumentManager::panicCount() vue par update(). Une
  // panique venue d'un transport MIDI (deconnexion BLE / rtpMIDI, CC120/123 en
  // serie) ne passe par aucun code web : sans cette observation, elle coupait
  // les actionneurs mais l'auto-calibration continuait et rouvrait la valve au
  // pas suivant. _panicCountSeen distingue "jamais observe" de "observe a 0" :
  // la premiere valeur est seulement memorisee, elle n'annule rien.
  uint32_t _lastPanicCount = 0;
  bool _panicCountSeen = false;

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

  // A "test note" preview plays a real timed note through the sequencer and is
  // stopped automatically after TEST_NOTE_DURATION_MS by update().
  uint8_t _testNoteMidi = 0;
  unsigned long _testNoteOffTime = 0;

  // Controlled restart: after a restart-required config change / reset, safe the
  // hardware and schedule a reboot so the change takes effect cleanly. While set,
  // config-mutating routes are refused so they cannot overwrite the pending config.
  unsigned long _pendingRestartTime = 0;
  void scheduleControlledRestart();
  bool restartPending() const { return _pendingRestartTime != 0; }

  // --- Upload MIDI -----------------------------------------------------------
  // Un unique slot d'upload, possede par UNE requete a la fois (verrou exclusif).
  // Les anciens membres partages du serveur (_uploadFile / _uploadSize /
  // _uploadFileName / _uploadError) n'appartenaient a personne : deux clients
  // simultanes ecrivaient dans le meme descripteur de fichier et se volaient le
  // nom de destination. Un second client recoit desormais 409 upload_busy sans
  // jamais toucher au transfert en cours ; un transfert abandonne (onglet ferme,
  // Wi-Fi coupe) est nettoye par update() apres UPLOAD_LOCK_TIMEOUT_MS.
  struct MidiUploadSlot {
    AsyncWebServerRequest* owner = nullptr;
    File file;
    size_t size = 0;
    String fileName;        // nom de destination assaini
    String tmpPath;         // fichier temporaire unique de ce transfert
    bool error = false;
    const char* errorCode = "";
    unsigned long lastActivity = 0;
  };
  MidiUploadSlot _upload;
  uint32_t _uploadSequence;         // suffixe unique des fichiers temporaires
  bool acquireUploadLock(AsyncWebServerRequest* request);
  void releaseUploadLock(AsyncWebServerRequest* request);
  void abandonStaleUpload(unsigned long now);

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

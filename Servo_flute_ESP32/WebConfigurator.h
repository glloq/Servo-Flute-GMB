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
#include "ConfigSnapshot.h"
#include "MidiFilePlayer.h"
#include "WebAuth.h"
#include "WebOpChannel.h"
#include "WsOpRing.h"

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

  /* ==========================================================================
   * PROPRIETE DES ETATS PARTAGES - a lire avant d'ajouter un membre
   * ==========================================================================
   * Deux taches se partagent cet objet. Une troisieme n'existe pas : TOUS les
   * callbacks d'ESPAsyncWebServer (HTTP et WebSocket) s'executent sur la MEME
   * tache AsyncTCP, donc deux callbacks web ne sont jamais concurrents entre
   * eux. La seule concurrence reelle est AsyncTCP <-> loop().
   *
   * Plutot qu'un mutex par membre - qui couterait cher sur des chemins
   * brulants et donnerait surtout l'illusion d'une protection - chaque etat a
   * un PROPRIETAIRE declare :
   *
   *   loop()    : actionneurs, `cfg`, LittleFS, lecteur MIDI, calibrateur.
   *               _micMonitorEnabled, _micMonitorBeforeCalibration, _rfDoneSent,
   *               _rfDoneTime, _autoCalOwnerClientId, _pendingRestartTime,
   *               _lastPanicCount, _panicCountSeen, _lastStatusBroadcast,
   *               _lastWsCleanup, _lastAudioBroadcast, _lastAcalBroadcast.
   *               AsyncTCP peut les LIRE : ce sont des scalaires alignes, la
   *               lecture ne fait jamais de lecture-modification-ecriture, et
   *               le pire cas d'une valeur d'une passe en retard est documente
   *               au-dessus du membre concerne.
   *
   *   AsyncTCP  : etat de TRANSPORT - _wsSessions, _auth, _upload,
   *               _webVelocity, _wsActuatorWindowStart, _wsActuatorCount,
   *               _uploadSequence. loop() n'y touche QUE depuis une operation
   *               publiee par runOnLoop(), c'est-a-dire pendant que la tache
   *               AsyncTCP est justement en train d'attendre ce resultat :
   *               l'exclusion est structurelle, pas accidentelle. Aucune
   *               operation POSTEE (postWebOp, chemin WebSocket non bloquant)
   *               n'a le droit d'y toucher - voir executeWebOp().
   *
   *   PARTAGE   : _op (propriete tournante, voir WebOpChannel),
   *               _calCancelRequested (_calCancelMux),
   *               session de test manuelle et note de test (_sessionMux).
   *               Chacun passe par une primitive qui rend la sequence
   *               lire-puis-ecrire indivisible - jamais par `volatile`, qui
   *               n'est pas une primitive de synchronisation.
   * ======================================================================== */

  // --- Hand-off AsyncTCP -> loop() -----------------------------------------
  // PROPRIETE : l'emplacement `_op` n'a pas de proprietaire fixe - il en change
  // au fil de l'operation, et c'est WebOpChannel qui dit lequel a chaque
  // instant (IDLE / ARMED / RUNNING / DONE, voir WebOpChannel.h). La regle
  // tient en une phrase : on n'ecrit ni ne lit `_op` sans tenir le jeton
  // (WebOpTicket cote producteur, WebOpClaim cote loop()) qui l'autorise.
  //
  // Les trois anciens drapeaux `volatile` - _opPending, _opAbandoned,
  // _opDoneSeq - ont disparu : `volatile` n'apporte ni atomicite ni barriere et
  // n'est pas une primitive de synchronisation inter-coeur. Les transitions
  // sont desormais prises dans une section critique, ce qui rend exclusives les
  // deux decisions qui s'annulent ("j'abandonne" / "je publie le resultat").
  WebOp _op;
  WebOpChannel _opChannel;
  // Verrou de PRODUCTEUR : serialise les appelants HTTP entre eux, tenu
  // pendant toute l'operation, bloquant.
  SemaphoreHandle_t _opMutex;
  // Semaphore binaire : signal de fin publie par loop().
  SemaphoreHandle_t _opDone;
  // Section critique MINUSCULE des transitions d'etat du canal. Un portMUX, pas
  // un mutex : on n'y fait que lire/ecrire quelques scalaires, jamais une
  // allocation ni un appel de controleur (les interruptions y sont masquees).
  portMUX_TYPE _opStateMux = portMUX_INITIALIZER_UNLOCKED;
  // Adaptateurs injectes dans WebOpChannel : le module reste pur, les vraies
  // primitives FreeRTOS vivent ici.
  static bool chanLockProducer(void* ctx, uint32_t timeoutMs);
  static void chanUnlockProducer(void* ctx);
  static void chanEnterState(void* ctx);
  static void chanExitState(void* ctx);
  static bool chanWaitDone(void* ctx, uint32_t timeoutMs);
  static void chanSignalDone(void* ctx);
  static void chanDrainDone(void* ctx);
  static uint32_t chanNowMs(void* ctx);
  static void chanYieldMs(void* ctx, uint32_t ms);
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
  // Les indices et la BORNE par passe vivent dans WsOpRing (pur, teste sur
  // hote). Seule la charge utile - qui porte des `String` Arduino - reste ici.
  WsOpRing _wsOpRing;
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
  //
  // PROPRIETE : pose par n'importe quelle tache (AsyncTCP, loop()), consomme
  // par loop() SEULE. Le drapeau etait `volatile`, ce qui ne donne ni atomicite
  // ni barriere : la sequence "si pose, l'effacer" tenait en deux acces
  // distincts et une demande deposee ENTRE les deux etait effacee sans avoir
  // ete traitee - une annulation perdue, c'est-a-dire des actionneurs qui
  // repartent apres un panic. Pose ET prise passent maintenant par la meme
  // section critique, exactement comme CommandQueue::takePanicRequest() et
  // InstrumentManager::takeResetControllersRequest().
  // La mecanique elle-meme vit dans LatchedRequest (WebOpChannel.h), PURE et
  // donc verifiable sur hote : c'est ce qui rend la correction demontrable au
  // lieu d'etre seulement relue.
  LatchedRequest _calCancelRequested;
  portMUX_TYPE _calCancelMux = portMUX_INITIALIZER_UNLOCKED;
  static void calCancelEnter(void* ctx);
  static void calCancelExit(void* ctx);
  void requestCalibrationCancel();
  // Lecture ET effacement INDIVISIBLES. Rend true si une annulation etait en
  // attente. Deux demandes coalescent en une seule prise ; une demande deposee
  // pendant le traitement de la precedente survit a ce traitement.
  bool takeCalibrationCancel();
  void executeWebOp(WebOp& op);
  // Libere les ressources portees par l'operation (candidat de configuration),
  // qu'elle ait ete appliquee ou abandonnee. Idempotent.
  static void releaseWebOp(WebOp& op);

  // --- Authentification ----------------------------------------------------
  // PROPRIETE : AsyncTCP. loop() n'appelle _auth que depuis
  // WEBOP_SET_ADMIN_PASSWORD et WEBOP_REGEN_AP_PASSWORD, qui sont publiees par
  // runOnLoop() (chemin HTTP) et jamais par postWebOp() : la tache AsyncTCP est
  // donc bloquee dans son attente pendant ces deux operations et ne peut pas
  // lire la table au meme instant. Cette exclusion est une PROPRIETE DU
  // CABLAGE, pas une garantie du type : poster l'une de ces operations par
  // postWebOp() la casserait en silence.
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
  // MEMES adaptateurs, vus par ConfigSnapshot : toute lecture AsyncTCP qui
  // porte sur plus d'un champ - ou sur un champ dont une valeur a moitie
  // remplacee serait visible - passe par ces primitives, jamais par une lecture
  // directe de `cfg`.
  ConfigLockOps configLockOps();
  // Copie coherente de la configuration active. Rend false si le verrou est
  // refuse : l'appelant HTTP repond alors `config_busy` / 503, comme
  // GET /api/config et GET /api/diagnostics le font deja.
  bool snapshotActiveConfig(RuntimeConfig& dst);
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
  //
  // PROPRIETE : etat de SESSION, ecrit par les DEUX taches - AsyncTCP l'ouvre
  // et le ferme depuis un message WebSocket, loop() le ferme sur expiration ou
  // sur panic. Il n'a donc pas de proprietaire unique possible sans changer le
  // protocole (beginTestSession doit repondre oui/non tout de suite au client).
  // Il est protege par _sessionMux, une section critique minuscule qui rend
  // INDIVISIBLES les sequences lire-puis-ecrire : "le test est-il libre ? alors
  // je le prends" et "l'extinction differee est-elle due ? alors je l'efface".
  // Aucune commande materielle n'est emise sous ce verrou.
  uint32_t _testOwnerClientId = 0;
  unsigned long _testStartTime = 0;
  bool _testActive = false;
  // `mutable` : testSessionExpired() et isTestOwner() sont const et doivent
  // pourtant entrer en section critique pour lire DEUX champs d'un coup. Meme
  // raison que InstrumentManager::_requestMux.
  mutable portMUX_TYPE _sessionMux = portMUX_INITIALIZER_UNLOCKED;
  // Start/refresh the manual-test window. Returns false (and changes nothing) if a
  // test is already active and owned by a different client, so ownership can't be
  // stolen mid-test.
  bool beginTestSession(uint32_t clientId);
  void endTestSession(bool safeHardware);     // stop it (optionally safing hardware)

  // --- C-1 : un ORDRE D'ARRET ne se perd pas -------------------------------
  // postCommand() rend false quand l'anneau est plein. Pour "pump_stop" et
  // "fan_stop", cette valeur etait IGNOREE et endTestSession(false) retirait
  // juste apres le filet de securite de la session (le plafond
  // TEST_SESSION_MAX_MS et l'extinction differee) : la pompe restait alimentee
  // SANS AUCUNE limite de temps. C'est le pire defaut connu de ce fichier.
  //
  // INVARIANT TENU ICI : quand une session de test est declaree terminee, la
  // remise en securite du materiel est DEJA garantie. Cette fonction poste
  // l'ordre d'arret demande ; s'il est refuse, elle ESCALADE en panic - un
  // drapeau dedie, hors anneau, qui ne peut pas etre perdu et qui vide les
  // commandes deja en attente. Dans les deux cas le materiel repart au repos.
  //
  // Rend true si l'ordre CIBLE a ete accepte, false s'il a fallu escalader
  // (l'appelant previent alors le client que tout a ete coupe, pas seulement
  // l'actionneur vise). Le contrat du LOT A - postCommand() rend toujours true
  // pour les ordres qui retirent de l'energie - rendra l'escalade inatteignable
  // en pratique ; la verification reste, c'est une defense en profondeur et non
  // une delegation.
  bool postStopCommand(uint8_t cmdType, uint8_t a = 0);
  // Vrai si une session est ouverte depuis plus de TEST_SESSION_MAX_MS. Les
  // deux champs sont lus dans la MEME section critique : sinon `_testActive`
  // pouvait etre vu a vrai avec un `_testStartTime` deja remis a zero par
  // l'autre tache, et le filet de securite se declenchait a contretemps.
  bool testSessionExpired(unsigned long now) const;
  // Vrai si le proprietaire de la session est ce client (lecture coherente).
  // Vrai si une session de test manuel est ouverte, QUEL QUE SOIT son
  // proprietaire. Sert a refuser le demarrage d'une calibration : un second
  // navigateur ne doit pas pouvoir en lancer une pendant le test du premier.
  bool testSessionActive() const;
  bool isTestOwner(uint32_t clientId) const;

  // A "test note" preview plays a real timed note through the sequencer and is
  // stopped automatically after TEST_NOTE_DURATION_MS by update().
  //
  // PROPRIETE : meme categorie, meme verrou (_sessionMux). La note et son
  // echeance forment UN couple : les lire separement permettait a un
  // `test_note` recu entre les deux de faire eteindre la mauvaise note - et,
  // pire, d'effacer l'echeance de la nouvelle, donc de laisser une note
  // SONNER indefiniment (air ouvert, plus aucune extinction programmee).
  uint8_t _testNoteMidi = 0;
  unsigned long _testNoteOffTime = 0;
  // Arme l'extinction differee d'une note de test (couple pose ensemble).
  void armTestNoteOff(uint8_t note, unsigned long offTime);
  // Prend l'extinction differee si elle est due : lecture ET effacement
  // indivisibles, meme discipline que takeCalibrationCancel().
  bool takeDueTestNoteOff(unsigned long now, uint8_t& note);

  // Controlled restart: after a restart-required config change / reset, safe the
  // hardware and schedule a reboot so the change takes effect cleanly. While set,
  // config-mutating routes are refused so they cannot overwrite the pending config.
  //
  // PROPRIETE : ecrit par loop() SEULE (scheduleControlledRestart() n'est
  // appelee que depuis executeWebOp()), lu par AsyncTCP via restartPending().
  // Un seul mot aligne, jamais de lecture-modification-ecriture cote lecteur :
  // le pire cas est une requete de configuration acceptee une passe trop tot,
  // et cette requete est elle-meme serialisee par le canal vers loop(), donc
  // executee APRES la programmation du redemarrage.
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

  // --- Remplacement MIDI transactionnel (FileTransaction.h) ------------------
  // Le remplacement etait DESTRUCTIF : `remove(dest)` puis `rename(tmp, dest)`.
  // Si le rename ET son repli par copie echouaient (LittleFS plein, secteur
  // fatigue), l'ancien fichier etait deja detruit et le nouveau n'arrivait
  // jamais : l'utilisateur perdait un morceau en televersant un morceau.
  // fileTxInstall() met l'ancien DE COTE au lieu de le supprimer, et le remet
  // en place si l'installation echoue.
  //
  // Le .bak vit a la RACINE, hors de MIDI_DIR, pour les memes raisons que le
  // .tmp d'upload : ni /api/midi/list ni getMidiStorageUsed() ne parcourent la
  // racine, donc aucun residu de transaction ne peut apparaitre dans la liste
  // ni etre compte dans le quota.
  static String midiBackupPathFor(const String& fileName);
  // Adaptateurs LittleFS injectes dans FileTransaction (module pur).
  static bool fsTxExists(void* ctx, const char* path);
  static bool fsTxRemove(void* ctx, const char* path);
  static bool fsTxRename(void* ctx, const char* from, const char* to);
  static bool fsTxCopy(void* ctx, const char* from, const char* to);
  // Chemin de DEMARRAGE : repare les installations coupees par une panne de
  // courant (un .bak orphelin a la racine = une destination peut-etre absente)
  // et balaie les temporaires d'upload que plus personne ne reclame.
  void recoverInterruptedMidiInstalls();

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

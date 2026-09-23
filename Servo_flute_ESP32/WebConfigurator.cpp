#include "WebConfigurator.h"

#include <new>   // std::nothrow : une allocation ratee doit rendre nullptr, pas abandonner
#include "gmb/GmbRuntime.h"
#include "InstrumentManager.h"
#include "FanController.h"
#include "WirelessManager.h"
#include "ConfigCommit.h"
#include "DeviceSecrets.h"
#include "web_content.h"
#include "WebValueParsers.h"
#if MIC_ENABLED
// Verdicts deja calcules par la chaine audio et simplement RELAYES ici. Inclus
// explicitement : ce fichier nomme AcousticClassification, QualityScore,
// NoteTiming et TimingOutcome, il ne doit pas dependre du fait qu'un autre
// en-tete les tire par hasard.
#include "AcousticQuality.h"
#include "AcousticTiming.h"
#endif
#include <WiFi.h>

// Serialize a single string value through ArduinoJson so quotes, backslashes and
// control characters in user-provided fields (device name, SSID, colour, ...) are
// correctly escaped. Returns the value WITH its surrounding double quotes, e.g.
// jsonStr("Flute \"A\"") -> "\"Flute \\\"A\\\"\"". Used by the hand-assembled JSON
// responses so those free-form fields cannot break the payload.
static String jsonStr(const char* s) {
  JsonDocument d;
  d.set(s ? s : "");
  String out;
  serializeJson(d, out);
  return out;
}

// Per-request POST body. Stored in AsyncWebServerRequest::_tempObject so concurrent
// requests on different routes (or two close requests) never share one buffer - the
// previous shared members could be overwritten, mixed, or read by the wrong route.
//
// IMPORTANT : ESPAsyncWebServer libere _tempObject avec free() dans le destructeur
// de la requete. L'objet doit donc etre un POD alloue avec malloc() : un objet C++
// contenant un String serait libere sans appeler son destructeur (fuite memoire a
// chaque requete interrompue) et free()-e alors qu'il vient de new.
struct WebReqBody {
  size_t capacity;
  size_t length;
  bool tooLarge;
  char data[1];   // tableau flexible : capacity+1 octets alloues a la suite
};

// Accumulate a chunked POST body into the request's own WebReqBody. Call from the
// route's body callback. The matching request handler takes ownership via
// takeRequestBody() below.
static void webAccumulateBody(AsyncWebServerRequest* request, uint8_t* data, size_t len,
                              size_t index, size_t total) {
  WebReqBody* b = (WebReqBody*)request->_tempObject;
  if (index == 0) {
    if (b) { free(b); request->_tempObject = nullptr; b = nullptr; }
    size_t capacity = (total > 0 && total <= (size_t)CONFIG_MAX_POST_BYTES)
                          ? total : (size_t)CONFIG_MAX_POST_BYTES;
    b = (WebReqBody*)malloc(sizeof(WebReqBody) + capacity);
    if (!b) return;
    b->capacity = capacity;
    b->length = 0;
    b->tooLarge = (total > (size_t)CONFIG_MAX_POST_BYTES);
    b->data[0] = '\0';
    request->_tempObject = b;
  }
  if (!b) return;
  if (b->tooLarge) return;
  if (b->length + len > b->capacity) {
    b->tooLarge = true;
    return;
  }
  memcpy(b->data + b->length, data, len);
  b->length += len;
  b->data[b->length] = '\0';
}

// Move the accumulated body out of the request into local variables and free the
// per-request buffer immediately, so no cleanup is needed on the handler's many
// early-return paths. Returns "" / false when there was no body.
static void takeRequestBody(AsyncWebServerRequest* request, String& outBody, bool& outTooLarge) {
  WebReqBody* b = (WebReqBody*)request->_tempObject;
  request->_tempObject = nullptr;
  if (b) {
    outTooLarge = b->tooLarge;
    outBody = b->tooLarge ? String() : String(b->data);
    free(b);
  } else {
    outBody = String();
    outTooLarge = false;
  }
}

// WS commands that drive an actuator open-endedly (until the user stops them) and
// therefore start/refresh a bounded manual-test session.
static bool isManualTestCommand(const char* type) {
  static const char* kTests[] = {
    "test_finger", "test_air", "test_angle", "angle_live", "test_sol",
    "test_note", "air_live", "pump_target", "fan_target"
  };
  for (const char* t : kTests) if (strcmp(type, t) == 0) return true;
  return false;
}

// WS commands submitted to the actuator rate limiter. Each one asks for MORE
// motion and losing one is harmless: the next slider event resends the position,
// and a dropped CC is exactly what the MIDI CC limiter already does.
//
// No STOPPING command is in this list, and that is the point. "nof", "panic",
// "stop", "pump_stop", "fan_stop", "auto_stop" - and "test_sol", whose o:0 is a
// valve close - always get through: throwing away an order to stop would be the
// exact opposite of what a rate limiter is for. The open direction of test_sol
// is not protected here either; it is protected at the source, by the re-pulse
// guard in AirflowController::testSolenoid() and by the absolute test-session
// ceiling, both of which hold at any message rate.
static bool isRateLimitedWsCommand(const char* type) {
  static const char* kLimited[] = {
    "non", "cc", "air_live", "angle_live", "test_finger", "test_air",
    "test_angle", "test_note", "pump_target", "fan_target"
  };
  for (const char* t : kLimited) if (strcmp(type, t) == 0) return true;
  return false;
}

// Reduit un nom de fichier recu du reseau a un nom simple et sur : pas de
// chemin, pas de "..", caracteres limites, extension .mid/.midi obligatoire.
static bool sanitizeMidiFileName(const String& raw, String& out) {
  String name = raw;
  int ls = name.lastIndexOf('/');
  if (ls >= 0) name = name.substring(ls + 1);
  int bs = name.lastIndexOf('\\');
  if (bs >= 0) name = name.substring(bs + 1);
  if (name.length() == 0 || name.length() > 48) return false;
  if (name.indexOf("..") >= 0) return false;
  if (name[0] == '.') return false;
  for (size_t i = 0; i < name.length(); i++) {
    char c = name[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_' || c == '.' || c == ' ' || c == '(' || c == ')';
    if (!ok) return false;
  }
  String lower = name;
  lower.toLowerCase();
  if (!lower.endsWith(".mid") && !lower.endsWith(".midi")) return false;
  out = name;
  return true;
}

WebConfigurator::WebConfigurator(uint16_t port)
  : _server(port), _ws("/ws"),
    _instrument(nullptr), _player(nullptr), _wirelessManager(nullptr),
    _webVelocity(WEB_DEFAULT_VELOCITY), _lastStatusBroadcast(0), _lastWsCleanup(0),
    _opPending(false), _opAbandoned(false), _opDoneSeq(0), _opSeqCounter(0),
    _opMutex(nullptr), _opDone(nullptr),
    _wsOpHead(0), _wsOpTail(0), _wsOpCount(0), _wsOpMutex(nullptr),
    _calCancelRequested(false), _cfgMutex(nullptr),
    _uploadSequence(0)
#if MIC_ENABLED
    , _audio(nullptr), _autoCal(nullptr), _micMonitorEnabled(false), _lastAudioBroadcast(0), _lastAcalBroadcast(0)
    , _autoCalOwnerClientId(0), _micMonitorBeforeCalibration(false), _rfDoneSent(false), _rfDoneTime(0)
#endif
{
  for (uint8_t i = 0; i < WS_MAX_CLIENTS; i++) {
    _wsSessions[i].clientId = 0;
    _wsSessions[i].token[0] = '\0';
  }
}

WebConfigurator::~WebConfigurator() {
#if MIC_ENABLED
  // Les DEUX observateurs pointent dans _audio : celui de chronometrie vise son
  // membre timing(), celui d'audio vise l'analyseur lui-meme. Les detacher
  // avant de detruire l'analyseur, sinon l'instrument garde un pointeur pendant
  // et la premiere note suivante ecrit dans de la memoire liberee.
  // WebConfigurator possede _audio ; InstrumentManager, lui, survit a sa
  // destruction. Poses ENSEMBLE dans begin(), retires ENSEMBLE ici : la duree
  // de vie qu'ils partagent est celle d'un seul objet.
  if (_instrument) {
    _instrument->setTimingObserver(nullptr);
    _instrument->setAudioObserver(nullptr);
  }
  delete _autoCal;
  delete _audio;
#endif
  if (_opDone) vSemaphoreDelete(_opDone);
  if (_opMutex) vSemaphoreDelete(_opMutex);
  if (_wsOpMutex) vSemaphoreDelete(_wsOpMutex);
  if (_cfgMutex) vSemaphoreDelete(_cfgMutex);
}

void WebConfigurator::begin(InstrumentManager* instrument, MidiFilePlayer* player) {
  _instrument = instrument;
  _player = player;

  // Hand-off vers la tache loop() : un mutex serialise les producteurs AsyncTCP,
  // un semaphore binaire signale la fin de l'execution cote loop().
  _opMutex = xSemaphoreCreateMutex();
  _opDone = xSemaphoreCreateBinary();
  _opPending = false;
  // File des commandes WebSocket : un MUTEX, pas un spinlock. Copier une WebOp
  // copie ses String, donc alloue sur le tas ; faire cela dans une section
  // critique (interruptions coupees) est interdit.
  _wsOpMutex = xSemaphoreCreateMutex();
  // Coherence de `cfg` entre le commit (tache loop) et les lecteurs AsyncTCP.
  _cfgMutex = xSemaphoreCreateMutex();
  // xSemaphoreCreateMutex() alloue : sur un tas epuise elle rend NULL. On retient
  // l'echec, parce qu'apres begin() un mutex absent ne veut PAS dire la meme
  // chose qu'avant - voir lockConfig().
  _cfgMutexFailed = (_cfgMutex == nullptr);
  _calCancelRequested = false;

  // Sessions web : jeton aleatoire tire du RNG materiel, expiration glissante.
  DeviceSecrets::begin();
  _auth.begin(DeviceSecrets::randomWord, WEB_SESSION_TTL_MS);

  // Configurer le WebSocket
  _ws.onEvent([this](AsyncWebSocket* server, AsyncWebSocketClient* client,
                     AwsEventType type, void* arg, uint8_t* data, size_t len) {
    onWsEvent(server, client, type, arg, data, len);
  });
  _server.addHandler(&_ws);

  // Creer le repertoire MIDI s'il n'existe pas
  if (!LittleFS.exists(MIDI_DIR)) {
    LittleFS.mkdir(MIDI_DIR);
  }

  // Configurer les routes HTTP
  setupRoutes();

  // Demarrer le serveur
  _server.begin();

  // GET /gmb/descriptor.json is now reachable: announce handshake flag bit 0.
  // In BLE mode this web server never starts and the flag stays clear, so the
  // firmware never advertises an HTTP route that does not exist.
  gmb::runtime::setHttpDescriptorAvailable(true);

#if MIC_ENABLED
  // Initialize microphone (INMP441 via I2S).
  //
  // `std::nothrow` sur les deux : tout le reste de ce fichier teste deja
  // `_audio` et `_autoCal` avant de les dereferencer (une douzaine de gardes
  // chacun), donc le mode "pas de micro, pas de calibration" est ecrit, teste,
  // et c'etait la seule chose qui ne pouvait pas l'atteindre. `micOk` reste
  // faux si l'analyseur n'a pas pu etre alloue, ce qui neutralise du meme coup
  // les deux observateurs poses plus bas.
  _audio = new (std::nothrow) AudioAnalyzer();
  bool micOk = (_audio != nullptr) && _audio->begin();
  if (micOk && _instrument) {
    _autoCal = new (std::nothrow) AutoCalibrator(
      _instrument->getFingerCtrl(),
      _instrument->getAirflowCtrl(),
      *_audio,
      _instrument->getCalibrationAirSupply());
    // PHASE 7 : la chaine d'actionneurs NOTIFIE ses instants d'ordre a la
    // chronometrie acoustique. C'est le seul point ou les deux mondes se
    // rencontrent, et le flux y est a SENS UNIQUE : l'instrument ecrit, il ne
    // lit jamais. Sans cet appel tout reste nullptr et l'instrument se comporte
    // exactement comme avant - c'est cette equivalence qui est testee
    // nativement par timing_observer_cannot_touch_actuators.
    //
    // Conditionne a `micOk` DELIBEREMENT, et pas seulement a la presence de
    // l'instrument : sans microphone, personne n'alimente la chronometrie en
    // frames, donc chaque note ouvrirait un cycle qui finirait en
    // TIMING_TIMEOUT. L'interface lirait "aucun son mesure" alors que la verite
    // est "personne n'ecoutait". Un repli muet vaut mieux qu'une mesure fausse.
    // Limite connue, partagee avec _autoCal juste au-dessus : un microphone
    // rebranche apres le demarrage ne rearme pas ce cablage.
    _instrument->setTimingObserver(&_audio->timing());
    // MEME cablage, MEME sens unique, pour le CHANGEMENT DE NOTE. Sans lui,
    // resetAcousticTracking() n'a aucun appelant sur le chemin de jeu - seule
    // une note VISEE la declenche, et seul AutoCalibrator en declare une - donc
    // la ligne de base de brillance et l'historique de pitch d'une note servent
    // a juger la suivante : ACOUSTIC_SQUEAK publie sur une octave montante
    // propre, `stability` a 0,00 avec `stabilityValid` vrai.
    //
    // POSE ET RETIRE AVEC L'AUTRE, toujours : les deux visent le meme _audio,
    // que le destructeur detruit. Les desapparier laisserait un pointeur
    // pendant.
    //
    // Conditionne au MEME `micOk`, mais pour une raison qui lui est propre :
    // sans microphone, aucune frame n'arrive et il n'y a aucun suivi acoustique
    // a remettre a zero. Poser l'observateur serait alors sans effet, jamais
    // dangereux - c'est la symetrie de pose/retrait qui commande ici, pas la
    // prudence.
    _instrument->setAudioObserver(_audio);
  }
  if (DEBUG) {
    Serial.print("DEBUG: WebConfigurator - Microphone INMP441: ");
    Serial.println(micOk ? "DETECTE" : "ABSENT");
  }
#endif

  if (DEBUG) {
    Serial.println("DEBUG: WebConfigurator - Serveur web demarre");
  }
}

void WebConfigurator::update() {
  unsigned long now = millis();

  // Executer, sur la tache loop(), les operations web en attente. C'est le SEUL
  // endroit ou une demande venue d'AsyncTCP fait modifier `cfg`, LittleFS, le
  // lecteur MIDI ou le calibrateur.
  serviceWsOps();      // commandes WebSocket (non bloquantes)
  servicePendingOp();  // requete HTTP en attente de sa reponse

  // UNE PANIQUE ANNULE L'AUTO-CALIBRATION, QUEL QU'EN SOIT LE CHEMIN.
  // requestCalibrationCancel() n'etait appelee que depuis des chemins WEB. Une
  // deconnexion BLE ou rtpMIDI, un CC120/123 recu en MIDI serie, un timeout
  // d'Active Sensing declenchent bien le panic d'InstrumentManager - les
  // actionneurs sont coupes - mais AutoCalibrator, qui ne passe par aucun code
  // web, reappliquait ses commandes au pas suivant (~740 ms) et defaisait la
  // mise en securite : valve rouverte, souffle relance, alors que plus aucun
  // transport ne repond.
  //
  // panicCount() est monotone. On compare par INEGALITE et non par ordre : un
  // debordement de uint32_t (sans objet en pratique) se comporte alors comme
  // n'importe quel autre changement, au lieu de rendre le compteur muet pour
  // toujours. La PREMIERE valeur vue est seulement memorisee : elle ne prouve
  // aucune panique recente (le compteur peut deja etre non nul quand le serveur
  // demarre) et aucune calibration ne tourne a cet instant.
  //
  // Place ici, AVANT la consommation de _calCancelRequested juste en dessous,
  // pour que l'annulation prenne effet dans le meme tour de update() - donc
  // avant _autoCal->update().
  if (_instrument) {
    const uint32_t panics = _instrument->panicCount();
    if (!_panicCountSeen) {
      _panicCountSeen = true;
      _lastPanicCount = panics;
    } else if (panics != _lastPanicCount) {
      _lastPanicCount = panics;
#if MIC_ENABLED
      requestCalibrationCancel();
#endif
      // La session de test manuelle ne possede plus rien : la panique a deja
      // remis les actionneurs au repos. `false` parce qu'en redemander un
      // second n'ajouterait rien - et parce que endTestSession() annule aussi
      // le note-off differe d'une note de test, qui n'a plus lieu d'etre.
      endTestSession(false);
    }
  }

#if MIC_ENABLED
  // Annulation de calibration NON PERDABLE. Elle transitait par postWebOp(), qui
  // echoue silencieusement quand la file est pleine : le panic coupait alors les
  // actionneurs mais _autoCal restait "running" et reappliquait ses commandes au
  // cycle suivant. Le drapeau, lui, ne peut pas etre perdu. Il est consomme ICI,
  // apres les operations web (un "start" poste avant l'annulation est donc bien
  // annule) et AVANT _autoCal->update() plus bas.
  if (_calCancelRequested) {
    _calCancelRequested = false;
    cancelActiveActuatorSession();
  }
#endif

  // Liberer un slot d'upload abandonne (client disparu en plein transfert).
  abandonStaleUpload(now);

  // Controlled restart after a restart-required config change / reset: the response
  // has been sent; reboot so the persisted config takes effect with a clean init.
  if (_pendingRestartTime != 0 && (int32_t)(now - _pendingRestartTime) >= 0) {
    ESP.restart();
  }

  // Server-side safety net for manual actuator tests: if the owning client stops
  // refreshing the session (tab suspended, browser crash, Wi-Fi lost, stop lost),
  // return the actuators to a safe state regardless of any browser-side timeout.
  if (_testActive && (now - _testStartTime) >= TEST_SESSION_MAX_MS) {
    endTestSession(true);
    _ws.textAll("{\"t\":\"test_expired\"}");
  }

  // Auto-stop a "test note" preview once its bounded duration has elapsed.
  if (_testNoteOffTime != 0 && (int32_t)(now - _testNoteOffTime) >= 0) {
    if (_instrument) _instrument->postCommand(ACMD_NOTE_OFF, _testNoteMidi);
    _testNoteOffTime = 0;
  }

  // Nettoyage periodique des clients WS deconnectes
  if (now - _lastWsCleanup >= WS_CLEANUP_INTERVAL_MS) {
    _ws.cleanupClients(WS_MAX_CLIENTS);
    _lastWsCleanup = now;
  }

  // Broadcast status periodique
  if (now - _lastStatusBroadcast >= WS_STATUS_INTERVAL_MS) {
    if (_ws.count() > 0) {
      broadcastStatus();
    }
    _lastStatusBroadcast = now;
  }

#if MIC_ENABLED
  // Update audio analyzer
  if (_audio && _audio->isMicDetected()) {
    // Declarer l'etat REEL de la source d'air AVANT l'analyse : le rapport
    // signal/bruit est calcule contre le profil de cet etat. Comparer une note
    // jouee pompe en marche a un plancher mesure pompe arretee surestimerait sa
    // qualite - or c'est le cas de TOUTES les notes en mode pompe.
    if (_instrument) {
      _audio->setAirSourceState(cfg.airMode,
                                _instrument->getPressureCtrl().getTargetPercent(),
                                _instrument->getFanCtrl().getSpeed());
    }
    _audio->update();

    // Une capture de bruit peut se terminer d'elle-meme au plafond de duree.
    // Sans cela, un onglet ferme en pleine capture laisserait le microphone et
    // tout le DSP tourner indefiniment.
    if (_audio->noiseCaptureFinished()) {
      const NoiseProfileId done = _audio->currentNoiseProfile();
      _audio->endNoiseCapture();
      _audio->setActive(_micMonitorEnabled || (_autoCal && _autoCal->isRunning()));
      if (_ws.count() > 0) {
        String nj = "{\"t\":\"noise\",\"ok\":true,\"auto_stopped\":1,\"profile\":\"";
        nj += NoiseModel::profileName(done);
        nj += "\"}";
        _ws.textAll(nj);
      }
    }

    // Broadcast audio data if monitoring enabled
    if (_micMonitorEnabled && _audio->isActive() && _ws.count() > 0) {
      if (now - _lastAudioBroadcast >= AUTOCAL_AUDIO_INTERVAL_MS) {
        // Descripteurs de la derniere frame analysee. Le debit reste limite par
        // AUTOCAL_AUDIO_INTERVAL_MS : on n'envoie jamais de PCM, seulement des
        // mesures deja reduites.
        const AcousticFeatures& af = _audio->getFeatures();
        String aj = "{\"t\":\"audio\"";
        // Le message se construit par une vingtaine de += successifs et part 10
        // fois par seconde. Sans reserve, chaque poussee enchaine autant de
        // reallocations du tas - et les verdicts ajoutes plus bas allongent
        // encore la chaine. Une seule allocation, dimensionnee au pire cas
        // mesure (322 octets, marge comprise), remplace la serie.
        //
        // BUDGET. Les deux drapeaux d'echelle et de poids ajoutes plus bas
        // ("hnr_sp", "brw") coutent 11 octets chacun, soit +22 sur ce pire
        // cas : 344 octets, et 370 en majorant CHAQUE champ a sa largeur
        // maximale (dBFS a -120,0, centroide a 5 chiffres, etat "wrong_note").
        // La reserve de 384 tient donc sans changer, et la poussee reste un
        // releve de descripteurs - pas un flux. Tout le reste (chronometrie
        // complete, compteurs de refus, drapeaux `missing` nommes) part
        // UNIQUEMENT sur GET /api/diagnostics, a la demande.
        aj.reserve(384);
        aj += ",\"rms\":" + String(_audio->getRMS(), 3);
        aj += ",\"snd\":" + String(_audio->isSoundDetected() ? 1 : 0);
        // Niveaux en dBFS : pleine echelle NUMERIQUE, jamais du dB SPL.
        aj += ",\"rms_dbfs\":" + String(af.rmsDbFS, 1);
        aj += ",\"peak_dbfs\":" + String(af.peakDbFS, 1);
        aj += ",\"clip\":" + String(af.clipping ? 1 : 0);
        aj += ",\"clip_ratio\":" + String(af.clippingRatio, 4);
        if (_audio->getPitchHz() > 0) {
          aj += ",\"hz\":" + String(_audio->getPitchHz(), 1);
          aj += ",\"midi\":" + String(_audio->getPitchMidi());
          aj += ",\"cents\":" + String(_audio->getPitchCents(), 1);
          aj += ",\"conf\":" + String((int)(_audio->getPitchConfidence() * 100.0f + 0.5f));
          // "valid" est le verdict du DETECTEUR DE PITCH, et rien d'autre :
          // il ne dit RIEN de la stabilite publiee juste en dessous.
          aj += ",\"valid\":" + String(_audio->isPitchValid() ? 1 : 0);
          // La stabilite ne part que MESUREE. Voir AcousticFeatures.h : 0
          // signifie "pas encore mesure" AUTANT que "tres instable". Il faut
          // MIC_PITCH_HISTORY frames d'historique (8 frames, soit 112 ms) pour
          // que le chiffre veuille dire quelque chose, alors que cette poussee
          // part toutes les AUTOCAL_AUDIO_INTERVAL_MS (100 ms) : la PREMIERE
          // poussee de chaque note porterait donc presque toujours "stab":0.00,
          // et un consommateur classerait chaque debut de note comme un defaut.
          // Omise plutot que renvoyee nue, comme les champs spectraux non
          // mesures plus bas : absente = pas mesuree, ce qui ne se confond avec
          // aucune valeur. Cout : -12 octets sur les frames concernees.
          if (af.stabilityValid) {
            aj += ",\"stab\":" + String(af.pitchStability, 2);
          }
        }
        // Champs spectraux UNIQUEMENT quand ils ont ete mesures : les omettre
        // vaut mieux que de renvoyer la valeur d'une frame anterieure.
        // Rapport signal/bruit mesure contre le profil de l'etat REEL. Omis
        // tant qu'aucun profil n'a ete capture ; le repli est signale pour que
        // l'interface ne presente pas un chiffre flatteur comme une mesure.
        if (af.snrValid) {
          aj += ",\"snr\":" + String(af.snrDb, 1);
          if (af.snrUsedFallback) aj += ",\"snr_fb\":1";
        }
        if (af.spectralValid) {
          aj += ",\"h2\":" + String(af.h2Ratio, 3);
          aj += ",\"h3\":" + String(af.h3Ratio, 3);
          // DEUX ECHELLES derriere une seule cle, donc JAMAIS le chiffre nu.
          // "hnr" est rempli soit par la mesure spectrale (FFT complete), soit
          // par l'APPROXIMATION Goertzel a quatre raies, qui compte tout
          // harmonique de rang > 4 comme du bruit : sur une meme note timbree
          // tenue, la mesure spectrale rend >= +31,86 dB la ou l'approximation
          // rend <= -0,02 dB, soit 31,88 dB d'ecart : les deux ne se comparent
          // pas et CLASSENT LES NOTES A L'ENVERS. La FFT ne tournant qu'une
          // frame sur MIC_SPECTRAL_DECIMATION, la cle alterne entre les deux
          // echelles a 15,6 Hz : sans ce drapeau, tout consommateur qui la
          // compare a un seuil voit son verdict clignoter.
          // Meme regle que hnr_is_spectral dans /api/diagnostics, et meme
          // exigence qu'AcousticQuality, qui refuse la composante HNR quand ce
          // drapeau est faux. Les deux champs partent ensemble ou pas du tout.
          // Cout : 11 octets.
          aj += ",\"hnr\":" + String(af.harmonicToNoiseRatio, 1);
          aj += ",\"hnr_sp\":" + String(af.hnrIsSpectral ? 1 : 0);
          // Uniquement si la FFT a tourne sur CETTE frame. Sinon ces deux
          // valeurs datent de la frame precedente (jusqu'a 64 ms) et les
          // envoyer comme une mesure courante serait faux.
          if (af.fftValid) {
            aj += ",\"centroid\":" + String(af.spectralCentroid, 0);
            aj += ",\"flatness\":" + String(af.spectralFlatness, 3);
          }
        }
        if (af.overblowDetected) aj += ",\"overblow\":1";

        // --- Classification et qualite (PHASES 6/7) : LE STRICT MINIMUM ------
        // Tout ce qui est ajoute ICI est multiplie par 10 par seconde et par le
        // nombre de clients (WS_MAX_CLIENTS = 4), soit 40 messages/s. Le releve
        // de chronometrie complet et les drapeaux `missing` NOMMES pesent a eux
        // seuls plus que la poussee entiere : les mettre la transformerait un
        // moniteur en flux permanent, ce que l'ESP32-WROOM ne doit pas soutenir
        // (cf. AUDIO_ARCHITECTURE.md). Ils partent donc UNIQUEMENT sur
        // GET /api/diagnostics, a la demande. Ne restent ici que les verdicts
        // qui changent a chaque frame et qu'un afficheur temps reel ne peut pas
        // reconstituer autrement.
        //
        // Lecture sans verrou, comme af juste au-dessus : cette fonction est
        // appelee par update(), donc par la tache loop(), qui est aussi la
        // SEULE a ecrire ces resultats (via _audio->update() quelques lignes
        // plus haut). Il n'y a pas d'autre ecrivain a serialiser, et aucun
        // calcul n'est declenche : on relit des champs deja poses.
        const AcousticClassification& cl = _audio->getClassification();
        // Etat omis tant qu'il n'a pas ete CLASSE : absent = inconnu, alors
        // qu'un "silence" par defaut se lirait comme une mesure.
        if (cl.classified) {
          aj += ",\"st\":\"" + String(_audio->getAcousticStateName()) + "\"";
        }
        // De QUOI la classification a ete privee, en un seul entier :
        //   bit 0 pitch, 1 snr, 2 spectre, 3 note visee, 4 stabilite,
        //   bit 5 historique de couac, 6 repli du SNR (compare au profil d'un
        //   AUTRE etat machine que l'etat reel : il surestime probablement la
        //   qualite).
        // Le detail nomme est dans /api/diagnostics (audio.missing) ; ici les
        // memes sept bits couteraient plus de 120 octets. Sans ce champ, un
        // etat "good" obtenu faute d'avoir rien pu mesurer serait, a l'ecran,
        // indiscernable d'un vrai "good". Omis quand rien ne manque.
        uint8_t miss = 0;
        if (cl.missingPitch)         miss |= 0x01;
        if (cl.missingSnr)           miss |= 0x02;
        if (cl.missingSpectrum)      miss |= 0x04;
        if (cl.missingExpectedNote)  miss |= 0x08;
        if (cl.missingStability)     miss |= 0x10;
        if (cl.missingSqueakHistory) miss |= 0x20;
        if (cl.snrUsedFallback)      miss |= 0x40;
        if (miss) aj += ",\"miss\":" + String((int)miss);
        // La note de qualite ne part JAMAIS sans le poids sur lequel elle a ete
        // calculee, ici comme dans /api/diagnostics : c'est une moyenne
        // ponderee de sept criteres dont l'attaque, qui peut ne pas avoir ete
        // mesuree ; le score est alors renormalise sur les six autres et ne
        // couvre que 90 % du cahier des charges. Les deux champs partent
        // ensemble ou pas du tout - un score seul mentirait sur ce qu'il
        // mesure.
        const QualityScore& qs = _audio->getQualityScore();
        if (qs.valid) {
          aj += ",\"q\":" + String(qs.score, 2);
          aj += ",\"qw\":" + String(qs.weightUsed, 2);
        }
        // MEME REGLE QUE POUR LA QUALITE, et pour une raison plus forte
        // encore : la respiration est une moyenne ponderee de trois
        // composantes (HNR, energie inter-harmonique, platitude) dont deux
        // n'existent qu'une frame sur MIC_SPECTRAL_DECIMATION. weightUsed
        // descend donc jusqu'a 0,30 - un ecart bien plus grand que celui du
        // score de qualite, qui ne descend qu'a 0,75. Sur une note tenue
        // immobile, "br" alterne ainsi entre une valeur pleine et 0,00 a
        // 62,5 Hz avec valid=true dans les deux cas : c'est le poids, et lui
        // seul, qui distingue "pas de souffle" de "presque rien de mesure".
        // Les deux champs partent ensemble ou pas du tout. Cout : 11 octets.
        const BreathinessResult& br = _audio->getBreathiness();
        if (br.valid) {
          aj += ",\"br\":" + String(br.value, 2);
          aj += ",\"brw\":" + String(br.weightUsed, 2);
        }
        // Couac CONFIRME seulement. Un candidat instantane est retire
        // retroactivement une fois sur deux : l'annoncer ferait clignoter
        // l'interface sur des evenements qui n'ont pas eu lieu.
        const SqueakResult& sq = _audio->getSqueak();
        if (sq.confirmed) aj += ",\"squeak\":1";

        aj += "}";
        _ws.textAll(aj);
        _lastAudioBroadcast = now;
      }
    }

    // Update auto-calibrator
    if (_autoCal && _autoCal->isRunning()) {
      // NE PAS reprendre la session ici. La prise de possession a lieu UNE fois
      // au demarrage (WEBOP_AUTOCAL_START_*), la liberation UNE fois a la fin
      // (cancelActiveActuatorSession). Repeter setActuatorSessionActive(true) a
      // chaque boucle relancait son effet de bord d'entree - arret du sequenceur
      // et purge de la file - ce qui, cote InstrumentManager, refermait la valve
      // ouverte par le calibrateur : l'auto-calibration ne mesurait jamais aucun
      // son. La fonction est desormais idempotente, mais l'appel repetitif reste
      // inutile et trompeur.
      _autoCal->update();

      // Broadcast progress
      if (_ws.count() > 0 && now - _lastAcalBroadcast >= AUTOCAL_AUDIO_INTERVAL_MS) {
        if (_autoCal->isRangeMode()) {
          // Range finder progress
          String pj = "{\"t\":\"rf_prog\"";
          pj += ",\"angle\":" + String(_autoCal->getCurrentAngle());
          if (_autoCal->getRangeFinderMin() >= 0) {
            pj += ",\"min\":" + String(_autoCal->getRangeFinderMin());
          }
          pj += "}";
          _ws.textAll(pj);
        } else {
          // Per-note airflow calibration progress
          int ni = _autoCal->getCurrentNoteIndex();
          byte midi = (ni >= 0 && ni < cfg.numNotes) ? cfg.notes[ni].midiNote : 0;
          String noteName = "";
          if (midi > 0) {
            const char* names[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
            noteName = String(names[midi % 12]) + String((int)(midi / 12) - 1);
          }
          String pj = "{\"t\":\"acal_prog\"";
          pj += ",\"idx\":" + String(ni);
          pj += ",\"note\":\"" + noteName + "\"";
          pj += ",\"total\":" + String(_autoCal->getTotalNotes());
          pj += ",\"phase\":\"" + String(_autoCal->getPhaseName()) + "\"";
          pj += ",\"air\":" + String(_autoCal->getCurrentAirPercent());
          pj += ",\"rms\":" + String(_autoCal->getLastRms(), 3);
          pj += ",\"noise\":" + String(_autoCal->getNoiseRms(), 3);
          pj += ",\"hz\":" + String(_autoCal->getLastHz(), 1);
          pj += ",\"midi\":" + String(_autoCal->getLastMidi());
          pj += ",\"cents\":" + String(_autoCal->getLastCents(), 1);
          pj += ",\"confidence\":" + String(_autoCal->getLastConfidence());
          pj += ",\"validFrames\":" + String(_autoCal->getLastValidFrames());
          pj += ",\"totalFrames\":" + String(_autoCal->getLastTotalFrames());
          pj += "}";
          _ws.textAll(pj);
        }
        _lastAcalBroadcast = now;
      }
    }

    // Global-timeout safety abort: notify the UI and restore the monitor state.
    if (_autoCal && _autoCal->takeTimeoutEvent()) {
      _ws.textAll("{\"t\":\"acal_error\",\"msg\":\"Timeout global de calibration\"}");
      _micMonitorEnabled = _micMonitorBeforeCalibration;
      _audio->setActive(_micMonitorEnabled);
      _autoCalOwnerClientId = 0;
      _rfDoneSent = false;
      if (_instrument) _instrument->setActuatorSessionActive(false);
    }

    // Check range finder completion. Broadcast rf_done ONCE and then KEEP the
    // calibrator in ACAL_RF_COMPLETE so the result stays applicable: apply_range
    // requires isRangeFinderComplete(), so we must not stop() here. The state is
    // cleared on apply / cancel / new start / owner disconnect.
    if (_autoCal && _autoCal->isRangeFinderComplete() && !_rfDoneSent) {
      bool ok = _autoCal->getRangeFinderMin() >= 0 && _autoCal->getRangeFinderMax() >= 0;
      String dj = "{\"t\":\"rf_done\",\"ok\":";
      dj += String(ok ? "true" : "false");
      if (ok) {
        dj += ",\"min\":" + String(_autoCal->getRangeFinderMin());
        dj += ",\"max\":" + String(_autoCal->getRangeFinderMax());
      } else {
        uint8_t reason = _autoCal->getRangeFailureReason();
        dj += ",\"reason\":" + String(reason);
        dj += ",\"reasonName\":\"" + String(AutoCalibrator::failureReasonName(reason)) + "\"";
        if (reason == ACAL_FAIL_AIR_SUPPLY) {
          dj += ",\"airError\":\"" +
                String(AutoCalibrator::airSupplyErrorName(_autoCal->getAirSupplyError())) + "\"";
        }
      }
      dj += "}";
      _ws.textAll(dj);
      _rfDoneSent = true;
      _rfDoneTime = now;
      // Restore the user's pre-calibration monitor preference; keep ownership so
      // only the owner can apply/cancel the pending result.
      _micMonitorEnabled = _micMonitorBeforeCalibration;
      if (_audio) _audio->setActive(_micMonitorEnabled);
    }
    // Auto-cancel a pending range-finder result the owner never resolved, so the
    // ownership / config lock / servo-power session cannot stay held indefinitely.
    if (_autoCal && _autoCal->isRangeFinderComplete() && _rfDoneSent &&
        (now - _rfDoneTime) >= AUTOCAL_RF_REVIEW_TIMEOUT_MS) {
      _ws.textAll("{\"t\":\"rf_expired\"}");
      cancelActiveActuatorSession();
    }
    // Check airflow calibration completion
    if (_autoCal && _autoCal->isComplete()) {
      // applyResults() only overwrites notes whose new calibration is valid (a
      // failed note keeps its previous configuration) and restores RAM on a
      // storage failure, reporting exactly what happened.
      AutoCalApplyResult ap = _autoCal->applyResults();
      const char* names[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
      // "ok" reflects the PERSISTED outcome: values must be both applied AND saved.
      // A storage failure rolls the RAM config back (applied==false), so a client
      // is never told success while nothing was actually written.
      bool okOverall = ap.applied && ap.saved;
      // A persisted calibration is an ACTIVE capability change: per-note airflow
      // windows decide which fingerings are announced as playable. The revision
      // only moves if the announced set really changed (a re-calibration that
      // lands on the same playable notes is a no-op for GMB).
      if (okOverall) gmb::runtime::onConfigurationActivated();
      String dj = "{\"t\":\"acal_done\"";
      dj += ",\"ok\":" + String(okOverall ? "true" : "false");
      dj += ",\"applied\":" + String(ap.applied ? "true" : "false");
      dj += ",\"saved\":" + String(ap.saved ? "true" : "false");
      if (ap.validCount > 0 && !ap.saved) dj += ",\"error\":\"storage_failed\"";
      dj += ",\"validCount\":" + String(ap.validCount);
      dj += ",\"failedCount\":" + String(ap.failedCount);
      dj += ",\"results\":[";
      for (int i = 0; i < cfg.numNotes; i++) {
        if (i > 0) dj += ",";
        byte midi = cfg.notes[i].midiNote;
        String nn = String(names[midi % 12]) + String((int)(midi / 12) - 1);
        AutoCalNoteResult r = _autoCal->getResult(i);
        int range = (int)cfg.servoAirflowMax - (int)cfg.servoAirflowMin;
        int minAngle = (int)cfg.servoAirflowMin + r.airMin * range / 100;
        int nomAngle = (int)cfg.servoAirflowMin + r.airNominal * range / 100;
        int maxAngle = (int)cfg.servoAirflowMin + r.airMax * range / 100;
        dj += "{\"name\":\"" + nn + "\",\"ok\":" + String(r.valid ? "true" : "false");
        dj += ",\"min\":" + String(r.airMin);
        dj += ",\"nominal\":" + String(r.airNominal);
        dj += ",\"max\":" + String(r.airMax);
        dj += ",\"confidence\":" + String(r.confidence);
        dj += ",\"cents\":" + String(r.medianCents, 1);
        dj += ",\"stability\":" + String(r.pitchStability, 2);
        dj += ",\"snr\":" + String(r.signalToNoiseRatio, 1);
        dj += ",\"reason\":" + String(r.failureReason);
        dj += ",\"reasonName\":\"" + String(AutoCalibrator::failureReasonName(r.failureReason)) + "\"";
        dj += ",\"minA\":" + String(minAngle);
        dj += ",\"nomA\":" + String(nomAngle);
        dj += ",\"maxA\":" + String(maxAngle) + "}";
      }
      dj += "]}";
      _ws.textAll(dj);
      _autoCal->stop();
      // Restore the user's pre-calibration monitor preference and resume power mgmt.
      _micMonitorEnabled = _micMonitorBeforeCalibration;
      if (_audio) _audio->setActive(_micMonitorEnabled);
      _autoCalOwnerClientId = 0;
      _rfDoneSent = false;
      if (_instrument) _instrument->setActuatorSessionActive(false);
    }
  }
#endif
}

void WebConfigurator::setWirelessManager(WirelessManager* wm) {
  _wirelessManager = wm;
}

/*******************************************************************************
 * Hand-off AsyncTCP -> loop()
 *
 * Les callbacks HTTP/WebSocket s'executent sur la tache AsyncTCP. Ils ne doivent
 * toucher NI `cfg`, NI LittleFS, NI un actionneur, NI le calibrateur : ces
 * ressources appartiennent a la tache loop(). Le callback prepare donc une
 * operation, la depose dans l'unique emplacement protege par _opMutex, puis
 * attend (au plus WEBOP_TIMEOUT_MS) que loop() l'execute et publie son resultat.
 *
 * Il n'y a pas d'interblocage possible : loop() n'attend jamais AsyncTCP.
 ******************************************************************************/

void WebConfigurator::releaseWebOp(WebOp& op) {
  // Le candidat de configuration est alloue sur le tas par le callback web et sa
  // propriete est transferee a la tache loop() des que l'operation est armee.
  // C'est donc TOUJOURS loop() qui le libere, qu'elle l'applique ou l'abandonne.
  if (op.candidate) {
    delete op.candidate;
    op.candidate = nullptr;
  }
}

bool WebConfigurator::runOnLoop(WebOp& op) {
  if (_opMutex == nullptr || _opDone == nullptr) return false;
  if (xSemaphoreTake(_opMutex, pdMS_TO_TICKS(WEBOP_TIMEOUT_MS)) != pdTRUE) return false;

  // Ne JAMAIS ecraser une operation que loop() serait encore en train d'executer :
  // l'emplacement est unique et il serait lu et ecrit en meme temps.
  unsigned long spinDeadline = millis() + WEBOP_TIMEOUT_MS;
  while (_opPending && (int32_t)(millis() - spinDeadline) < 0) {
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  if (_opPending) {
    xSemaphoreGive(_opMutex);
    return false;   // l'appelant garde la propriete de ce qu'il portait
  }

  uint32_t seq = ++_opSeqCounter;
  if (seq == 0) seq = ++_opSeqCounter;   // 0 est reserve a "aucune operation"

  // Vider un eventuel signal de fin laisse par une operation abandonnee, sinon
  // l'attente ci-dessous retournerait immediatement avec un resultat etranger.
  xSemaphoreTake(_opDone, 0);

  _op = op;
  _op.seq = seq;
  _op.ok = false;
  _op.httpStatus = 200;
  _op.json = "";
  _opAbandoned = false;
  _opPending = true;
  // Propriete transferee : l'appelant ne doit plus liberer le candidat.
  op.candidate = nullptr;

  bool done = false;
  unsigned long deadline = millis() + WEBOP_TIMEOUT_MS;
  while (true) {
    int32_t remaining = (int32_t)(deadline - millis());
    if (remaining <= 0) break;
    if (xSemaphoreTake(_opDone, pdMS_TO_TICKS(remaining)) != pdTRUE) break;
    // Ne retenir que la fin de NOTRE operation (une operation precedemment
    // abandonnee peut avoir signale sa fin entre-temps).
    if (_opDoneSeq == seq) { done = true; break; }
  }

  if (done) {
    op = _op;
  } else {
    // loop() n'a pas repondu a temps. On marque l'operation abandonnee : si elle
    // n'a pas encore demarre, loop() la liberera sans l'appliquer ; si elle a
    // demarre, elle ira a son terme mais personne n'attendra son resultat.
    _opAbandoned = true;
  }
  xSemaphoreGive(_opMutex);
  return done;
}

// Cette file etait protegee par un portMUX (spinlock + interruptions coupees).
// Or une WebOp porte trois String : la copier ALLOUE sur le tas, et l'allocateur
// prend lui-meme un verrou - operation interdite en section critique, qui peut
// bloquer ou corrompre le tas. Un mutex FreeRTOS autorise l'allocation ; la
// contention est negligeable (AsyncTCP produit, loop() consomme, la garde dure
// le temps d'une copie).
bool WebConfigurator::postWebOp(const WebOp& op) {
  if (_wsOpMutex == nullptr) return false;
  if (xSemaphoreTake(_wsOpMutex, pdMS_TO_TICKS(WEBOP_QUEUE_LOCK_MS)) != pdTRUE) return false;
  if (_wsOpCount >= kWsOpQueueSize) {
    xSemaphoreGive(_wsOpMutex);
    return false;
  }
  _wsOps[_wsOpHead] = op;
  _wsOpHead = (uint8_t)((_wsOpHead + 1) % kWsOpQueueSize);
  _wsOpCount++;
  xSemaphoreGive(_wsOpMutex);
  return true;
}

void WebConfigurator::serviceWsOps() {
  if (_wsOpMutex == nullptr) return;
  while (true) {
    WebOp op;
    if (xSemaphoreTake(_wsOpMutex, pdMS_TO_TICKS(WEBOP_QUEUE_LOCK_MS)) != pdTRUE) return;
    if (_wsOpCount == 0) {
      xSemaphoreGive(_wsOpMutex);
      return;
    }
    op = _wsOps[_wsOpTail];
    _wsOps[_wsOpTail] = WebOp();
    _wsOpTail = (uint8_t)((_wsOpTail + 1) % kWsOpQueueSize);
    _wsOpCount--;
    xSemaphoreGive(_wsOpMutex);

    executeWebOp(op);
    // Le demandeur n'attend pas : son resultat eventuel part sur le WebSocket.
    if (op.json.length() > 0) _ws.textAll(op.json);
    releaseWebOp(op);
  }
}

void WebConfigurator::servicePendingOp() {
  if (!_opPending) return;
  bool abandoned = _opAbandoned;
  if (!abandoned) executeWebOp(_op);
  releaseWebOp(_op);
  uint32_t seq = _op.seq;
  _opPending = false;
  _opDoneSeq = seq;
  // Ne signaler que si quelqu'un attend encore ce resultat.
  if (!abandoned && _opDone) xSemaphoreGive(_opDone);
}

/*******************************************************************************
 * Authentification
 ******************************************************************************/

// La table ne memorisait que l'identifiant du client : une fois le "auth"
// initial accepte, plus rien ne reinterrogeait WebAuth. Une socket laissee
// ouverte restait donc authentifiee bien au-dela du TTL de session, et survivait
// meme a une revocation globale des jetons. On memorise desormais le JETON, et
// chaque commande le revalide : l'expiration s'applique, la fenetre glisse comme
// pour HTTP, et revokeAll() coupe effectivement les WebSockets.
bool WebConfigurator::isWsAuthenticated(uint32_t clientId) {
  if (clientId == 0) return false;
  for (uint8_t i = 0; i < WS_MAX_CLIENTS; i++) {
    if (_wsSessions[i].clientId != clientId) continue;
    if (_auth.validate(String(_wsSessions[i].token), millis())) return true;
    // Jeton expire ou revoque : la socket redevient anonyme et devra se
    // reauthentifier ({"t":"auth","token":"..."}).
    _wsSessions[i].clientId = 0;
    _wsSessions[i].token[0] = '\0';
    return false;
  }
  return false;
}

void WebConfigurator::setWsAuthenticated(uint32_t clientId, const String& token) {
  if (clientId == 0) return;
  clearWsAuthentication(clientId);
  if (token.length() == 0 || token.length() > WEB_AUTH_TOKEN_LEN) return;
  uint8_t slot = WS_MAX_CLIENTS;
  for (uint8_t i = 0; i < WS_MAX_CLIENTS; i++) {
    if (_wsSessions[i].clientId == 0) { slot = i; break; }
  }
  // Table pleine : recycler la premiere entree (les clients WS sont limites a
  // WS_MAX_CLIENTS par cleanupClients()).
  if (slot >= WS_MAX_CLIENTS) slot = 0;
  _wsSessions[slot].clientId = clientId;
  strncpy(_wsSessions[slot].token, token.c_str(), WEB_AUTH_TOKEN_LEN);
  _wsSessions[slot].token[WEB_AUTH_TOKEN_LEN] = '\0';
}

void WebConfigurator::clearWsAuthentication(uint32_t clientId) {
  if (clientId == 0) return;
  for (uint8_t i = 0; i < WS_MAX_CLIENTS; i++) {
    if (_wsSessions[i].clientId != clientId) continue;
    _wsSessions[i].clientId = 0;
    _wsSessions[i].token[0] = '\0';
  }
}

bool WebConfigurator::lockConfig(uint32_t timeoutMs) {
  if (_cfgMutex != nullptr) {
    return xSemaphoreTake(_cfgMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
  }
  // Pas de mutex : deux situations tres differentes, qu'il ne faut pas
  // confondre comme le faisait la version precedente.
  //
  //  - AVANT begin() : un seul contexte touche `cfg`, il n'y a rien a
  //    serialiser, et repondre "verrouille" est exact.
  //  - APRES begin() : la creation du semaphore a ECHOUE faute de tas, et
  //    pourtant les taches AsyncTCP, elles, tournent. Repondre "verrouille"
  //    laissait alors ecrire `cfg` sans aucune protection TOUT EN ANNONCANT le
  //    contraire a ConfigCommit - un verrou qui ment est pire qu'un verrou
  //    absent, parce que l'appelant cesse de se mefier.
  //
  // On echoue donc en FERMETURE : lockConfig() rend false, l'appelant HTTP
  // repond une erreur et le commit refuse d'activer plutot que d'ecrire `cfg`
  // a l'aveugle.
  return !_cfgMutexFailed;
}

void WebConfigurator::unlockConfig() {
  if (_cfgMutex) xSemaphoreGive(_cfgMutex);
}

String WebConfigurator::extractToken(AsyncWebServerRequest* request) const {
  if (request->hasHeader("X-Auth-Token")) {
    return request->getHeader("X-Auth-Token")->value();
  }
  if (request->hasParam("token")) {
    return request->getParam("token")->value();
  }
  return String();
}

bool WebConfigurator::rejectIfUnauthorized(AsyncWebServerRequest* request) {
  String token = extractToken(request);
  if (_auth.validate(token, millis())) return false;
  request->send(401, "application/json", "{\"ok\":false,\"error\":\"unauthorized\"}");
  return true;
}

void WebConfigurator::handleApiLogin(AsyncWebServerRequest* request) {
  String body; bool tooLarge;
  takeRequestBody(request, body, tooLarge);
  if (tooLarge || body.length() == 0) {
    request->send(400, "application/json", "{\"ok\":false,\"error\":\"bad_request\"}");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    request->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_json\"}");
    return;
  }
  String password = doc["password"] | "";
  if (!DeviceSecrets::verifyAdminPassword(password)) {
    // Pas de detail sur la raison de l'echec.
    request->send(401, "application/json", "{\"ok\":false,\"error\":\"invalid_credentials\"}");
    return;
  }
  JsonDocument resp;
  resp["ok"] = true;
  resp["token"] = _auth.createSession(millis());
  resp["ttl_ms"] = (uint32_t)WEB_SESSION_TTL_MS;
  String out;
  serializeJson(resp, out);
  request->send(200, "application/json", out);
}

void WebConfigurator::handleApiAuthStatus(AsyncWebServerRequest* request) {
  JsonDocument doc;
  doc["auth_required"] = true;
  doc["authenticated"] = _auth.validate(extractToken(request), millis());
  doc["default_password"] = DeviceSecrets::adminPasswordIsGenerated();
  String out;
  serializeJson(doc, out);
  request->send(200, "application/json", out);
}

void WebConfigurator::handleApiAuthPassword(AsyncWebServerRequest* request) {
  String body; bool tooLarge;
  takeRequestBody(request, body, tooLarge);
  if (rejectIfUnauthorized(request)) return;
  if (tooLarge || body.length() == 0) {
    request->send(400, "application/json", "{\"ok\":false,\"error\":\"bad_request\"}");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    request->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_json\"}");
    return;
  }
  WebOp op;
  op.type = WEBOP_SET_ADMIN_PASSWORD;
  op.strA = String((const char*)(doc["password"] | ""));
  if (op.strA.length() < 8) {
    request->send(400, "application/json", "{\"ok\":false,\"error\":\"password_too_short\"}");
    return;
  }
  if (!runOnLoop(op)) {
    request->send(503, "application/json", "{\"ok\":false,\"error\":\"busy\"}");
    return;
  }
  request->send(op.httpStatus, "application/json", op.json);
}

/*******************************************************************************
 * Protection hardware_not_ready
 *
 * isHardwareReady() existait deja mais plusieurs commandes web attaquaient
 * directement getAirflowCtrl() / getPressureCtrl() / getFanCtrl() / le
 * calibrateur, contournant les gardes de noteOn()/setPWM(). Le refus est
 * desormais applique en DEUX endroits complementaires :
 *   - ici, pour repondre explicitement "hardware_not_ready" au client,
 *   - dans InstrumentManager::applyCommand(), qui est le seul chemin
 *     d'application et refuse toute commande physique quoi qu'il arrive.
 * Apres un echec PCA0/PCA1, aucun actionneur n'est donc activable, par aucun
 * chemin. Diagnostics, lecture/modification de configuration, reset et
 * recovery reseau restent disponibles.
 ******************************************************************************/

bool WebConfigurator::hardwareReady() const {
  return _instrument != nullptr && _instrument->isHardwareReady();
}

bool WebConfigurator::rejectIfHardwareNotReady(AsyncWebServerRequest* request) {
  if (hardwareReady()) return false;
  request->send(503, "application/json", "{\"ok\":false,\"error\":\"hardware_not_ready\"}");
  return true;
}

bool WebConfigurator::isPhysicalWsCommand(const char* type) {
  // Toute commande qui met un actionneur en mouvement : test de doigt, test de
  // souffle, angle de servo, solenoide, pompe, ventilateur, note de test, note
  // jouee, CC expressif, calibration automatique et range finder.
  static const char* kPhysical[] = {
    "non", "nof", "cc", "air_live", "angle_live",
    "test_finger", "test_air", "test_angle", "test_sol", "test_note",
    "pump_target", "pump_enable", "fan_target", "play", "auto_cal", "auto_stop"
  };
  for (const char* t : kPhysical) if (strcmp(type, t) == 0) return true;
  return false;
}

/*******************************************************************************
 * Verrou d'upload MIDI exclusif
 ******************************************************************************/

bool WebConfigurator::acquireUploadLock(AsyncWebServerRequest* request) {
  if (_upload.owner != nullptr && _upload.owner != request) return false;
  _upload.owner = request;
  _upload.lastActivity = millis();
  return true;
}

void WebConfigurator::releaseUploadLock(AsyncWebServerRequest* request) {
  if (_upload.owner != request) return;
  if (_upload.file) _upload.file.close();
  if (_upload.tmpPath.length() > 0 && LittleFS.exists(_upload.tmpPath)) {
    LittleFS.remove(_upload.tmpPath);
  }
  _upload.owner = nullptr;
  _upload.tmpPath = "";
  _upload.fileName = "";
  _upload.size = 0;
  _upload.error = false;
  _upload.errorCode = "";
}

void WebConfigurator::abandonStaleUpload(unsigned long now) {
  // Transfert interrompu (onglet ferme, Wi-Fi coupe) : le slot serait sinon
  // bloque pour toujours et le fichier temporaire resterait sur LittleFS.
  if (_upload.owner == nullptr) return;
  if ((now - _upload.lastActivity) < UPLOAD_LOCK_TIMEOUT_MS) return;
  if (DEBUG) Serial.println("DEBUG: WebConfigurator - upload abandonne, slot libere");
  AsyncWebServerRequest* owner = _upload.owner;
  releaseUploadLock(owner);
}

/*******************************************************************************
 * Execution des operations web - TACHE loop() UNIQUEMENT
 ******************************************************************************/

// Adaptateurs pour ConfigCommitGuard : ConfigCommit reste pur (aucun appel
// FreeRTOS), le verrou reel vit ici.
bool WebConfigurator::cfgGuardLock(void* ctx) {
  return static_cast<WebConfigurator*>(ctx)->lockConfig(WEB_CONFIG_LOCK_MS);
}
void WebConfigurator::cfgGuardUnlock(void* ctx) {
  static_cast<WebConfigurator*>(ctx)->unlockConfig();
}

void WebConfigurator::executeWebOp(WebOp& op) {
  const ConfigCommitGuard cfgGuard{ &WebConfigurator::cfgGuardLock,
                                    &WebConfigurator::cfgGuardUnlock, this };
  switch (op.type) {
    case WEBOP_COMMIT_CONFIG: {
      // Commit TRANSACTIONNEL (voir ConfigCommit.h) : valider -> sauvegarder ->
      // commit atomique. Aucun etat intermediaire n'est jamais visible.
      if (op.candidate == nullptr) {
        op.ok = false; op.httpStatus = 500;
        op.json = "{\"ok\":false,\"error\":\"internal\"}";
        break;
      }
      ConfigCommitResult res =
          commitCandidateConfig(cfg, *op.candidate, _instrument, &ConfigStorage::saveFrom,
                                &cfgGuard);

      JsonDocument resp;
      if (!res.valid) {
        resp["ok"] = false;
        resp["msg"] = "Invalid configuration";
        resp["error"] = res.error;
        op.httpStatus = 400;
        op.ok = false;
      } else if (!res.saved) {
        // Sauvegarde impossible : ni la configuration active ni les controleurs
        // n'ont bouge. L'appareil continue sur la configuration persistee.
        resp["ok"] = false;
        resp["saved"] = false;
        resp["applied"] = false;
        resp["restart_required"] = res.restartRequired;
        resp["error"] = res.error;
        op.httpStatus = 500;
        op.ok = false;
      } else {
        resp["ok"] = true;
        resp["saved"] = true;
        resp["applied"] = res.applied;
        // `activated` porte litteralement l'invariant du commit : la
        // configuration ACTIVE a-t-elle ete remplacee ? Le client le deduisait
        // de `applied` + `restart_required` ; l'exposer evite la deduction.
        resp["activated"] = res.activated;
        resp["restart_required"] = res.restartRequired;
        resp["corrected"] = res.corrected;
        JsonArray reinit = resp["reinitialized"].to<JsonArray>();
        int start = 0;
        while (start < (int)res.reinitialized.length()) {
          int comma = res.reinitialized.indexOf(',', start);
          if (comma < 0) comma = res.reinitialized.length();
          if (comma > start) reinit.add(res.reinitialized.substring(start, comma));
          start = comma + 1;
        }
        JsonArray warnings = resp["warnings"].to<JsonArray>();
        if (res.warnings.length() > 0) warnings.add(res.warnings);

        if (res.restartRequired) {
          // La nouvelle configuration est sauvegardee mais PAS active : elle
          // demande une re-init hardware. On met les actionneurs en securite et
          // on programme un reboot controle ; l'ancienne configuration reste
          // active jusque-la, donc les controleurs restent coherents avec le
          // hardware reellement initialise.
          scheduleControlledRestart();
        } else if (res.activated) {
          // Configuration validee, sauvegardee ET active : c'est seulement ici
          // que la revision General-Midi-Boop peut avancer.
          gmb::runtime::onConfigurationActivated();
        }
        resp["restarting"] = restartPending();
        op.ok = true;
        op.httpStatus = 200;
      }
      serializeJson(resp, op.json);
      delete op.candidate;
      op.candidate = nullptr;
      break;
    }

    case WEBOP_RESET_CONFIG:
    case WEBOP_FACTORY_RESET: {
      // Mettre le hardware en securite PENDANT que `cfg` correspond encore a lui :
      // allSoundOff() lit cfg.airMode pour choisir quel sous-systeme d'air arreter.
      if (_instrument) _instrument->allSoundOff();
      bool ok = (op.type == WEBOP_RESET_CONFIG) ? ConfigStorage::resetToDefaults()
                                                : ConfigStorage::factoryReset();
      JsonDocument resp;
      resp["ok"] = ok;
      resp["restart_required"] = true;
      resp["restarting"] = ok;
      if (!ok) resp["error"] = "storage_failed";
      serializeJson(resp, op.json);
      op.ok = ok;
      op.httpStatus = ok ? 200 : 500;
      if (ok) scheduleControlledRestart();
      break;
    }

    case WEBOP_RESTART: {
      if (_instrument) _instrument->allSoundOff();
      op.ok = true;
      op.json = "{\"ok\":true,\"msg\":\"Restarting\"}";
      scheduleControlledRestart();
      break;
    }

    case WEBOP_FORMAT_FS: {
      // Action DESTRUCTIVE et volontaire (mode recovery). Jamais automatique.
      if (_instrument) _instrument->allSoundOff();
      bool ok = ConfigStorage::formatFilesystem();
      JsonDocument resp;
      resp["ok"] = ok;
      resp["fs_status"] = (int)ConfigStorage::filesystemStatus();
      if (!ok) resp["error"] = ConfigStorage::filesystemError();
      resp["restarting"] = ok;
      serializeJson(resp, op.json);
      op.ok = ok;
      op.httpStatus = ok ? 200 : 500;
      if (ok) scheduleControlledRestart();
      break;
    }

    case WEBOP_WIFI_CONNECT: {
      // Les identifiants sont d'abord persistes de facon transactionnelle, puis
      // la bascule reseau est demandee. startSTA() effectue le panic + demontage.
      RuntimeConfig candidate = cfg;
      strncpy(candidate.wifiSsid, op.strA.c_str(), sizeof(candidate.wifiSsid) - 1);
      candidate.wifiSsid[sizeof(candidate.wifiSsid) - 1] = '\0';
      strncpy(candidate.wifiPassword, op.strB.c_str(), sizeof(candidate.wifiPassword) - 1);
      candidate.wifiPassword[sizeof(candidate.wifiPassword) - 1] = '\0';
      ConfigCommitResult res =
          commitCandidateConfig(cfg, candidate, _instrument, &ConfigStorage::saveFrom,
                                &cfgGuard);
      JsonDocument resp;
      resp["ok"] = res.saved;
      if (!res.saved) {
        resp["error"] = res.valid ? "storage_failed" : res.error;
      } else if (!res.activated) {
        // Persiste mais PAS actif : le verrou de configuration a ete refuse,
        // donc `cfg` porte encore les ANCIENS identifiants. Ne PAS basculer le
        // reseau ici, pour deux raisons d'inegale gravite :
        //  - connectToNetwork() partirait sur les nouveaux identifiants pendant
        //    que la configuration active en decrit d'autres ;
        //  - surtout, le prochain POST /api/config construit son candidat a
        //    partir de `cfg`. Il reecrirait donc en flash les ANCIENS
        //    identifiants, effacant en silence ceux qu'on vient d'enregistrer.
        //    Une perte de donnees silencieuse, pas une incoherence d'affichage.
        // Le redemarrage controle recharge la flash et remet RAM et flash
        // d'accord ; la bascule reseau se fera au boot sur les bons.
        resp["msg"] = "Saved, restarting";
        resp["restart_required"] = true;
        scheduleControlledRestart();
      } else {
        resp["msg"] = "Connecting...";
      }
      serializeJson(resp, op.json);
      op.ok = res.saved;
      op.httpStatus = res.saved ? 200 : 500;
      if (res.saved && res.activated && _wirelessManager) {
        _wirelessManager->getWifiMidi().connectToNetwork(op.strA.c_str(), op.strB.c_str());
      }
      break;
    }

    case WEBOP_MIDI_DELETE: {
      String path = String(MIDI_DIR) + "/" + op.strA;
      JsonDocument resp;
      if (!LittleFS.exists(path)) {
        resp["ok"] = false;
        resp["msg"] = "File not found";
        op.httpStatus = 404;
        op.ok = false;
      } else {
        // Ne jamais supprimer le fichier en cours de lecture sans arreter le
        // lecteur : il lirait un descripteur invalide.
        if (_player && _player->isFileLoaded() && _player->getFileName() == op.strA) {
          _player->stop();
        }
        bool removed = LittleFS.remove(path);
        resp["ok"] = removed;
        if (!removed) { resp["msg"] = "Delete failed"; op.httpStatus = 500; }
        resp["used"] = getMidiStorageUsed();
        resp["limit"] = (size_t)cfg.midiStorageLimitKb * 1024;
        op.ok = removed;
      }
      serializeJson(resp, op.json);
      break;
    }

    case WEBOP_MIDI_FINALIZE: {
      // ORDRE VOLONTAIRE : on VALIDE d'abord le fichier televerse (taille, quota,
      // parsing MIDI reel) et on ne remplace le fichier existant qu'ensuite.
      // L'ancien code renommait le temporaire par-dessus la destination AVANT de
      // le parser : un fichier valide etait donc detruit par un upload invalide.
      JsonDocument resp;
      String destPath = String(MIDI_DIR) + "/" + _upload.fileName;
      size_t limitBytes = (size_t)cfg.midiStorageLimitKb * 1024;

      // Quota : la taille du fichier remplace ne compte pas deux fois.
      size_t currentUsed = getMidiStorageUsed();
      size_t existingSize = 0;
      if (LittleFS.exists(destPath)) {
        File ef = LittleFS.open(destPath, "r");
        if (ef) { existingSize = ef.size(); ef.close(); }
      }
      if (currentUsed - existingSize + _upload.size > limitBytes) {
        resp["ok"] = false;
        resp["error"] = "storage_full";
        resp["msg"] = "MIDI storage full";
        resp["used"] = currentUsed;
        resp["limit"] = limitBytes;
        serializeJson(resp, op.json);
        op.ok = false;
        op.httpStatus = 400;
        break;
      }

      // Validation du CONTENU sur le fichier temporaire, avant tout remplacement.
      if (!_player || !_player->loadFile(_upload.tmpPath.c_str())) {
        resp["ok"] = false;
        resp["error"] = "invalid_midi";
        resp["msg"] = "Invalid MIDI format";
        resp["reason"] = _player ? _player->getLoadErrorCode() : "no_player";
        serializeJson(resp, op.json);
        op.ok = false;
        op.httpStatus = 400;
        break;   // le fichier existant est intact
      }

      if (!LittleFS.exists(MIDI_DIR)) LittleFS.mkdir(MIDI_DIR);

      // Le contenu est valide : on peut maintenant remplacer la destination.
      if (LittleFS.exists(destPath)) LittleFS.remove(destPath);
      bool moved = LittleFS.rename(_upload.tmpPath, destPath);
      if (!moved) {
        // Repli : copie manuelle (certaines versions de LittleFS ESP32).
        File src = LittleFS.open(_upload.tmpPath, "r");
        File dst = LittleFS.open(destPath, "w");
        if (src && dst) {
          uint8_t buf[512];
          moved = true;
          while (src.available()) {
            size_t n = src.read(buf, sizeof(buf));
            if (dst.write(buf, n) != n) { moved = false; break; }
          }
          dst.close();
          src.close();
          if (moved) LittleFS.remove(_upload.tmpPath);
          else LittleFS.remove(destPath);
        } else {
          if (src) src.close();
          if (dst) dst.close();
        }
      }
      if (!moved) {
        resp["ok"] = false;
        resp["error"] = "storage_error";
        resp["msg"] = "MIDI file storage error";
        serializeJson(resp, op.json);
        op.ok = false;
        op.httpStatus = 500;
        break;
      }

      // Recharger depuis la destination definitive pour que le lecteur pointe sur
      // le fichier final (et non sur le temporaire qui vient de disparaitre).
      //
      // `_player` est teste : le meme gestionnaire le teste deja plus haut
      // (chemin du temporaire), mais pas ici. L'asymetrie etait sans
      // consequence tant qu'un lecteur absent faisait redemarrer la carte a
      // l'allocation ; maintenant qu'un tas insuffisant le laisse a nullptr,
      // c'est un dereferencement atteignable depuis une requete web.
      bool loaded = _player && _player->loadFile(destPath.c_str());
      resp["ok"] = loaded;
      resp["file"] = _upload.fileName;
      if (loaded) {
        resp["events"] = _player->getEventCount();
        resp["duration"] = _player->getDurationMs();
        resp["channels"] = _player->getActiveChannels();
      } else {
        resp["error"] = "invalid_midi";
        resp["reason"] = _player ? _player->getLoadErrorCode() : "no_player";
      }
      resp["storage_used"] = getMidiStorageUsed();
      resp["storage_limit"] = limitBytes;
      serializeJson(resp, op.json);
      op.ok = loaded;
      op.httpStatus = loaded ? 200 : 400;

      if (loaded) {
        JsonDocument ws;
        ws["t"] = "midi_loaded";
        ws["file"] = _upload.fileName;
        ws["events"] = _player->getEventCount();
        ws["duration"] = _player->getDurationMs();
        ws["channels"] = _player->getActiveChannels();
        String wsMsg;
        serializeJson(ws, wsMsg);
        _ws.textAll(wsMsg);
      }
      break;
    }

    case WEBOP_MIDI_LOAD: {
      String path = String(MIDI_DIR) + "/" + op.strA;
      JsonDocument resp;
      if (!LittleFS.exists(path)) {
        resp["ok"] = false;
        resp["msg"] = "File not found";
        op.httpStatus = 404;
        op.ok = false;
      } else if (_player && _player->loadFile(path.c_str())) {
        resp["ok"] = true;
        resp["events"] = _player->getEventCount();
        resp["duration"] = _player->getDurationMs();
        resp["file"] = _player->getFileName();
        resp["channels"] = _player->getActiveChannels();
        op.ok = true;
        JsonDocument ws;
        ws["t"] = "midi_loaded";
        ws["file"] = _player->getFileName();
        ws["events"] = _player->getEventCount();
        ws["duration"] = _player->getDurationMs();
        ws["channels"] = _player->getActiveChannels();
        String wsMsg;
        serializeJson(ws, wsMsg);
        _ws.textAll(wsMsg);
      } else {
        resp["ok"] = false;
        resp["msg"] = "MIDI load failed";
        resp["reason"] = _player ? _player->getLoadErrorCode() : "no_player";
        op.httpStatus = 400;
        op.ok = false;
      }
      serializeJson(resp, op.json);
      break;
    }

    case WEBOP_PLAYER_PLAY:      if (_player) _player->play(); op.ok = true; break;
    case WEBOP_PLAYER_PAUSE:     if (_player) _player->pause(); op.ok = true; break;
    case WEBOP_PLAYER_STOP:      if (_player) _player->stop(); op.ok = true; break;
    case WEBOP_PLAYER_CH_FILTER:
      if (_player) _player->setChannelFilter(op.intA > 15 ? 255 : (uint8_t)op.intA);
      op.ok = true;
      break;

#if MIC_ENABLED
    case WEBOP_AUTOCAL_START_AIR:
    case WEBOP_AUTOCAL_START_RANGE: {
      if (!_autoCal || !_audio || !_audio->isMicDetected()) {
        op.ok = false; op.json = "{\"t\":\"acal_error\",\"msg\":\"no_microphone\"}";
        break;
      }
      if (_autoCal->isRunning()) {
        op.ok = false; op.json = "{\"t\":\"acal_error\",\"msg\":\"calibration_busy\"}";
        break;
      }
      // Suspendre la lecture MIDI : sinon le lecteur continuerait a pousser des
      // notes vers les actionneurs que la calibration s'apprete a posseder.
      if (_player) _player->pause();
      cancelActiveActuatorSession();
      _autoCalOwnerClientId = op.clientId;
      _micMonitorBeforeCalibration = _micMonitorEnabled;
      _rfDoneSent = false;
      _audio->setActive(true);
      if (_instrument) _instrument->setActuatorSessionActive(true);
      _autoCal->start(op.type == WEBOP_AUTOCAL_START_AIR ? ACAL_MODE_AIRFLOW : ACAL_MODE_RANGE_FIND);
      op.ok = true;
      break;
    }

    case WEBOP_AUTOCAL_APPLY_RANGE: {
      if (!_autoCal || !_autoCal->isRangeFinderComplete()) { op.ok = false; break; }
      bool hadValid = _autoCal->getRangeFinderMin() >= 0 && _autoCal->getRangeFinderMax() >= 0;
      RangeApplyResult ra = _autoCal->applyRangeResults();
      JsonDocument resp;
      resp["t"] = "rf_applied";
      if (ra.applied && ra.saved) {
        // Persiste et actif : GMB peut relire les capacites.
        gmb::runtime::onConfigurationActivated();
        resp["ok"] = true;
        resp["min"] = ra.minAngle;
        resp["max"] = ra.maxAngle;
        op.ok = true;
      } else {
        resp["ok"] = false;
        resp["error"] = hadValid ? "storage_failed" : "no_valid_range";
        op.ok = false;
      }
      serializeJson(resp, op.json);
      cancelActiveActuatorSession();
      break;
    }

    case WEBOP_MIC_MONITOR:
      _micMonitorEnabled = (op.intA != 0);
      if (_audio) _audio->setActive(_micMonitorEnabled || (_autoCal && _autoCal->isRunning()));
      op.ok = true;
      break;

    // --- Capture d'un profil de bruit (PHASE 5) ------------------------------
    // L'utilisateur amene d'abord l'instrument dans l'etat voulu (pompe a tel
    // regime, ventilateur a tel autre) avec les commandes de test existantes,
    // puis demande la capture. L'analyseur ecrit dans le profil correspondant a
    // l'etat REEL, lu a chaque passage d'update().
    case WEBOP_NOISE_START: {
      JsonDocument resp;
      resp["t"] = "noise";
      if (!_audio || !_audio->isMicDetected()) {
        resp["ok"] = false; resp["error"] = "no_microphone";
        op.ok = false;
      } else if (_instrument && _instrument->getSequencer().getState() != STATE_IDLE) {
        // Capturer un plancher de bruit pendant qu'une note sonne mesurerait la
        // note, pas le bruit. Le refus est explicite.
        resp["ok"] = false; resp["error"] = "note_playing";
        op.ok = false;
      } else {
        _audio->setActive(true);
        _audio->beginNoiseCapture();
        resp["ok"] = true;
        resp["capturing"] = NoiseModel::profileName(_audio->currentNoiseProfile());
        op.ok = true;
      }
      serializeJson(resp, op.json);
      break;
    }

    case WEBOP_NOISE_STOP: {
      JsonDocument resp;
      resp["t"] = "noise";
      const bool stored = _audio ? _audio->endNoiseCapture() : false;
      resp["ok"] = stored;
      if (!stored) {
        // Une capture trop courte est rejetee plutot que rangee comme un profil
        // de confiance douteuse.
        resp["error"] = "too_short";
        resp["min_frames"] = MIC_NOISE_MIN_FRAMES;
      } else {
        const NoiseProfileId id = _audio->currentNoiseProfile();
        const NoiseProfile& p = _audio->getNoiseModel().profile(id);
        resp["profile"] = NoiseModel::profileName(id);
        resp["frames"] = p.frames;
        resp["rms_dbfs"] = p.rmsDbFS;
        resp["flatness"] = p.flatness;
      }
      if (_audio) _audio->setActive(_micMonitorEnabled || (_autoCal && _autoCal->isRunning()));
      serializeJson(resp, op.json);
      op.ok = stored;
      break;
    }

    case WEBOP_NOISE_RESET: {
      if (_audio) _audio->resetNoiseModel();
      JsonDocument resp;
      resp["t"] = "noise";
      resp["ok"] = true;
      resp["msg"] = "noise profiles cleared";
      serializeJson(resp, op.json);
      op.ok = true;
      break;
    }

    case WEBOP_MIC_RESET: {
      JsonDocument resp;
      resp["t"] = "mic_reset";
      bool ok = _audio ? _audio->resetMicrophone() : false;
      resp["ok"] = ok;
      resp["status"] = _audio ? _audio->getMicStatusString() : "not_init";
      serializeJson(resp, op.json);
      op.ok = ok;
      break;
    }
#endif

    case WEBOP_SET_ADMIN_PASSWORD: {
      bool ok = DeviceSecrets::setAdminPassword(op.strA);
      JsonDocument resp;
      resp["ok"] = ok;
      if (!ok) resp["error"] = "password_too_short";
      serializeJson(resp, op.json);
      op.ok = ok;
      op.httpStatus = ok ? 200 : 400;
      // Les sessions ouvertes sont revoquees : le changement de secret doit
      // invalider les jetons distribues sous l'ancien.
      if (ok) {
        _auth.revokeAll();
        for (uint8_t i = 0; i < WS_MAX_CLIENTS; i++) {
          _wsSessions[i].clientId = 0;
          _wsSessions[i].token[0] = '\0';
        }
      }
      break;
    }

    case WEBOP_REGEN_AP_PASSWORD: {
      String pass = DeviceSecrets::regenerateApPassword();
      JsonDocument resp;
      resp["ok"] = true;
      resp["msg"] = "Hotspot key regenerated; printed on the serial console";
      serializeJson(resp, op.json);
      Serial.print("WifiMidiHandler - nouvelle cle hotspot: ");
      Serial.println(pass);
      op.ok = true;
      break;
    }

    default:
      op.ok = false;
      op.httpStatus = 400;
      op.json = "{\"ok\":false,\"error\":\"unknown_operation\"}";
      break;
  }

  if (op.json.length() == 0) {
    op.json = op.ok ? "{\"ok\":true}" : "{\"ok\":false}";
  }
}

bool WebConfigurator::beginTestSession(uint32_t clientId) {
  // Ownership cannot be stolen: while a manual test is active and owned by another
  // client, refuse a competing client's test command so two browsers can never
  // fight over the actuators. The owner may keep refreshing its own session.
  if (_testActive && _testOwnerClientId != 0 && clientId != _testOwnerClientId) {
    return false;
  }
  // PLAFOND ABSOLU, PAS GLISSANT. _testStartTime etait repose a CHAQUE commande
  // de test : le filet de securite de TEST_SESSION_MAX_MS etait donc repousse
  // par le flot qu'il est cense arreter. Un client qui envoie test_sol toutes
  // les 40 ms - ce qu'un simple glissement de curseur produit deja, et ce qu'un
  // client malveillant produit a volonte - ne voyait JAMAIS la coupure des 30 s.
  // L'horodatage n'est pose qu'a l'OUVERTURE de la session : 30 s apres la
  // premiere commande, update() remet le materiel en securite, quoi qu'il
  // arrive ensuite.
  // Ce que cela coute : un reglage qui dure plus de 30 s d'affilee est
  // interrompu une fois par ce retour au repos, et la commande suivante ouvre
  // simplement une nouvelle session. C'est le compromis voulu - un mode degrade
  // doit etre sur, pas pratique.
  if (!_testActive) _testStartTime = millis();
  _testOwnerClientId = clientId;
  _testActive = true;
  return true;
}

bool WebConfigurator::allowActuatorCommand(unsigned long now) {
  // Fenetre fixe, comme le limiteur de CC d'InstrumentManager : au premier
  // message hors fenetre on repart a zero. La soustraction non signee rend le
  // repliement de millis() inoffensif.
  if (now - _wsActuatorWindowStart >= WS_ACTUATOR_RATE_WINDOW_MS) {
    _wsActuatorWindowStart = now;
    _wsActuatorCount = 0;
  }
  if (_wsActuatorCount >= WS_ACTUATOR_RATE_LIMIT_PER_SECOND) return false;
  _wsActuatorCount++;
  return true;
}

void WebConfigurator::endTestSession(bool safeHardware) {
  // Le panic est POSTE : endTestSession() est appelee aussi bien depuis update()
  // (tache loop()) que depuis un evenement WebSocket (tache AsyncTCP), et seul
  // loop() a le droit de piloter les actionneurs.
  if (safeHardware && _instrument) _instrument->requestPanic();
  _testActive = false;
  _testOwnerClientId = 0;
  _testNoteOffTime = 0;   // cancel any pending test-note auto-stop
}

void WebConfigurator::scheduleControlledRestart() {
  // Return the hardware to a safe state now; the reboot (in update()) then reloads
  // the persisted config with the matching hardware initialisation.
  // Appelee uniquement depuis executeWebOp() (tache loop()), donc l'appel direct
  // a allSoundOff() est legitime et immediat.
  if (_instrument) _instrument->allSoundOff();
  if (_pendingRestartTime == 0) _pendingRestartTime = millis() + CONFIG_RESTART_DELAY_MS;
}

#if MIC_ENABLED
bool WebConfigurator::isCalibrationActive() const {
  // Active while measuring AND during the range-finder review window (the result
  // is still pending apply/cancel), so config stays locked until it is resolved.
  return _autoCal && (_autoCal->isRunning() || _autoCal->isRangeFinderComplete());
}

bool WebConfigurator::rejectIfCalibrationActive(AsyncWebServerRequest* request) {
  if (!isCalibrationActive()) return false;
  request->send(409, "application/json", "{\"ok\":false,\"error\":\"calibration_active\"}");
  return true;
}

void WebConfigurator::cancelActiveActuatorSession() {
  // Clean up a running calibration OR a pending completed/range-finder result.
  if (_autoCal && (_autoCal->isRunning() || _autoCal->isComplete() ||
                   _autoCal->isRangeFinderComplete())) {
    _autoCal->stop();
    // Restore the user's pre-calibration monitor choice (never leave the mic on).
    _micMonitorEnabled = _micMonitorBeforeCalibration;
    if (_audio) _audio->setActive(_micMonitorEnabled);
    _autoCalOwnerClientId = 0;
    _rfDoneSent = false;
  }
  // Allow normal power management to resume.
  if (_instrument) _instrument->setActuatorSessionActive(false);
}

bool WebConfigurator::actuatorCommandBlockedDuringCalibration(AsyncWebSocketClient* client, const char* type) {
  if (!isCalibrationActive()) return false;
  // Commands that would move actuators or cut the shared air source while the
  // calibration owns them. pump_stop / fan_stop are refused here (use panic or
  // auto_cal stop to abort a calibration); "stop" is handled specially so it
  // cancels the calibration cleanly rather than fighting it with allSoundOff.
  static const char* kBlocked[] = {
    "non", "nof", "cc", "air_live", "test_finger", "test_air", "test_angle",
    "angle_live", "test_sol", "test_note", "pump_target", "pump_enable", "fan_target",
    "pump_stop", "fan_stop", "play", "mic_mon", "mic_reset"
  };
  for (const char* b : kBlocked) {
    if (strcmp(type, b) == 0) {
      client->text("{\"t\":\"error\",\"msg\":\"calibration_active\"}");
      return true;
    }
  }
  return false;
}
#endif

void WebConfigurator::setupRoutes() {
  // MODELE D'AUTORISATION
  // ---------------------
  // Ouvert (purement informatif, ne modifie rien et ne bouge aucun actionneur) :
  //   GET /, /api/status, /api/config, /api/diagnostics, /api/wifi/status,
  //   /gmb/descriptor.json, /api/auth/status, POST /api/auth/login, captive portal.
  // Protege par jeton de session (X-Auth-Token ou ?token=) :
  //   toute modification de configuration, reset, redemarrage, formatage,
  //   gestion des fichiers MIDI, scan/connexion Wi-Fi, changement de mot de passe.
  // Le WebSocket exige un {"t":"auth","token":"..."} avant toute commande.

  // Page principale
  _server.on("/", HTTP_GET, [this](AsyncWebServerRequest* request) {
    handleRoot(request);
  });

  // --- Authentification ---
  _server.on("/api/auth/status", HTTP_GET, [this](AsyncWebServerRequest* request) {
    handleApiAuthStatus(request);
  });
  _server.on("/api/auth/login", HTTP_POST,
    [this](AsyncWebServerRequest* request) { handleApiLogin(request); },
    NULL,
    [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
      webAccumulateBody(request, data, len, index, total);
    }
  );
  _server.on("/api/auth/password", HTTP_POST,
    [this](AsyncWebServerRequest* request) { handleApiAuthPassword(request); },
    NULL,
    [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
      webAccumulateBody(request, data, len, index, total);
    }
  );
  // Regeneration volontaire de la cle du hotspot (affichee sur le port serie).
  _server.on("/api/auth/hotspot", HTTP_POST, [this](AsyncWebServerRequest* request) {
    if (rejectIfUnauthorized(request)) return;
    WebOp op;
    op.type = WEBOP_REGEN_AP_PASSWORD;
    if (!runOnLoop(op)) {
      request->send(503, "application/json", "{\"ok\":false,\"error\":\"busy\"}");
      return;
    }
    request->send(op.httpStatus, "application/json", op.json);
  });

  // API Status (informatif)
  _server.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest* request) {
    handleApiStatus(request);
  });

  // API Config GET (lecture : aucun secret n'y figure)
  _server.on("/api/config", HTTP_GET, [this](AsyncWebServerRequest* request) {
    handleApiConfig(request);
  });

  // API Config POST (body handler accumule, request handler traite)
  _server.on("/api/config", HTTP_POST,
    [this](AsyncWebServerRequest* request) {
      if (rejectIfUnauthorized(request)) return;
      handleApiConfigFinalize(request);
    },
    NULL,
    [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
      webAccumulateBody(request, data, len, index, total);
    }
  );

  // API Config Reset
  _server.on("/api/config/reset", HTTP_POST, [this](AsyncWebServerRequest* request) {
    if (rejectIfUnauthorized(request)) return;
    handleApiConfigReset(request);
  });

  // API Factory Reset (supprime le fichier config pour relancer le wizard)
  _server.on("/api/config/factory", HTTP_POST, [this](AsyncWebServerRequest* request) {
    if (rejectIfUnauthorized(request)) return;
    handleApiFactoryReset(request);
  });

  // Recovery LittleFS : formatage VOLONTAIRE uniquement. Le boot ne formate
  // jamais automatiquement (voir ConfigStorage::beginFilesystem). La requete doit
  // porter {"confirm":"format"} pour eviter tout declenchement accidentel.
  _server.on("/api/fs/format", HTTP_POST,
    [this](AsyncWebServerRequest* request) {
      String body; bool tooLarge;
      takeRequestBody(request, body, tooLarge);
      if (rejectIfUnauthorized(request)) return;
      JsonDocument doc;
      if (tooLarge || deserializeJson(doc, body) ||
          String((const char*)(doc["confirm"] | "")) != "format") {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"confirmation_required\","
                      "\"msg\":\"POST {\\\"confirm\\\":\\\"format\\\"} to erase the filesystem\"}");
        return;
      }
      WebOp op;
      op.type = WEBOP_FORMAT_FS;
      if (!runOnLoop(op)) {
        request->send(503, "application/json", "{\"ok\":false,\"error\":\"busy\"}");
        return;
      }
      request->send(op.httpStatus, "application/json", op.json);
    },
    NULL,
    [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
      webAccumulateBody(request, data, len, index, total);
    }
  );

  // API WiFi Scan (lance le scan)
  _server.on("/api/wifi/scan", HTTP_GET, [this](AsyncWebServerRequest* request) {
    if (rejectIfUnauthorized(request)) return;
    if (_wirelessManager) {
      _wirelessManager->getWifiMidi().startWifiScan();
      request->send(200, "application/json", "{\"ok\":true,\"msg\":\"Scan lance\"}");
    } else {
      request->send(500, "application/json", "{\"ok\":false}");
    }
  });

  // API WiFi Scan Results
  _server.on("/api/wifi/results", HTTP_GET, [this](AsyncWebServerRequest* request) {
    if (rejectIfUnauthorized(request)) return;
    if (_wirelessManager) {
      bool done = _wirelessManager->getWifiMidi().isScanComplete();
      // Les SSID viennent du reseau : la serialisation est faite par ArduinoJson
      // cote WifiMidiHandler, on n'insere ici qu'un document deja echappe.
      String json = "{\"done\":";
      json += done ? "true" : "false";
      if (done) {
        json += ",\"networks\":";
        json += _wirelessManager->getWifiMidi().getScanResultsJson();
      }
      json += "}";
      request->send(200, "application/json", json);
    } else {
      request->send(500, "application/json", "{\"ok\":false}");
    }
  });

  // API WiFi Connect (POST JSON {"ssid":"...","pass":"..."})
  _server.on("/api/wifi/connect", HTTP_POST,
    [this](AsyncWebServerRequest* request) {
      if (rejectIfUnauthorized(request)) return;
      handleApiWifiConnect(request);
    },
    NULL,
    [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
      webAccumulateBody(request, data, len, index, total);
    }
  );

  // API WiFi Status (informatif)
  _server.on("/api/wifi/status", HTTP_GET, [this](AsyncWebServerRequest* request) {
    JsonDocument doc;
    if (_wirelessManager) {
      WifiMidiHandler& wm = _wirelessManager->getWifiMidi();
      doc["state"] = (int)wm.getState();
      doc["ip"] = wm.getIPAddress();
      doc["ap"] = wm.isAPMode();
      doc["ssid"] = cfg.wifiSsid;   // echappe par ArduinoJson
      if (wm.getState() == WIFI_STATE_STA_CONNECTED) doc["rssi"] = WiFi.RSSI();
    }
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  // MIDI file list
  _server.on("/api/midi/list", HTTP_GET, [this](AsyncWebServerRequest* request) {
    if (rejectIfUnauthorized(request)) return;
    handleMidiList(request);
  });

  // MIDI file delete
  _server.on("/api/midi/delete", HTTP_POST,
    [this](AsyncWebServerRequest* request) {
      if (rejectIfUnauthorized(request)) return;
      handleMidiDelete(request);
    },
    NULL,
    [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
      webAccumulateBody(request, data, len, index, total);
    }
  );

  // MIDI file load (select for playback)
  _server.on("/api/midi/load", HTTP_POST,
    [this](AsyncWebServerRequest* request) {
      if (rejectIfUnauthorized(request)) return;
      handleMidiLoad(request);
    },
    NULL,
    [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
      webAccumulateBody(request, data, len, index, total);
    }
  );

  // Upload MIDI (multipart) - MUST be registered AFTER /api/midi/* sub-routes
  // because ESPAsyncWebServer prefix-matches /api/midi to /api/midi/*
  _server.on("/api/midi", HTTP_POST,
    [this](AsyncWebServerRequest* request) {
      handleMidiUploadComplete(request);
    },
    [this](AsyncWebServerRequest* request, const String& filename,
           size_t index, uint8_t* data, size_t len, bool final) {
      // L'autorisation est verifiee des le premier fragment : un client non
      // authentifie n'ecrit jamais le moindre octet sur LittleFS.
      if (index == 0) {
        String token = extractToken(request);
        if (!_auth.validate(token, millis())) {
          if (acquireUploadLock(request)) {
            _upload.error = true;
            _upload.errorCode = "unauthorized";
          }
          return;
        }
      }
      handleMidiUpload(request, filename, index, data, len, final);
    }
  );

  // Safe restart API (redemarrage controle par la tache loop()).
  _server.on("/api/restart", HTTP_POST, [this](AsyncWebServerRequest* request) {
    if (rejectIfUnauthorized(request)) return;
    WebOp op;
    op.type = WEBOP_RESTART;
    if (!runOnLoop(op)) {
      request->send(503, "application/json", "{\"ok\":false,\"error\":\"busy\"}");
      return;
    }
    request->send(op.httpStatus, "application/json", op.json);
  });

  // Hardware diagnostics API (passif, jamais de mouvement d'actionneur)
  _server.on("/api/diagnostics", HTTP_GET, [this](AsyncWebServerRequest* request) {
    handleApiDiagnostics(request);
  });
  _server.on("/api/diagnostics/run", HTTP_POST, [this](AsyncWebServerRequest* request) {
    handleApiDiagnostics(request);
  });

  // GET /gmb/descriptor.json - General-Midi-Boop v2 capability descriptor.
  // Served from the SAME cached document as the SysEx block 0x10 transfer, so the
  // two can never diverge. Advertised by handshake flag bit 0, which is only set
  // while this web server is running (Wi-Fi mode).
  _server.on("/gmb/descriptor.json", HTTP_GET, [](AsyncWebServerRequest* request) {
    // Copied into the response body on purpose: the cached document can be
    // replaced by a configuration activation while this async response is still
    // being written, and a zero-copy buffer would dangle when the old one is
    // released. The descriptor is ~1 kB, so the copy is cheap and this route is
    // control-plane traffic, not the note path.
    request->send(200, "application/json", String(gmb::runtime::descriptorJson().c_str()));
  });

  // Captive portal detection endpoints (mode AP)
  _server.on("/generate_204", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->redirect("http://192.168.4.1/");
  });
  _server.on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->redirect("http://192.168.4.1/");
  });
  _server.on("/connecttest.txt", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->redirect("http://192.168.4.1/");
  });
  _server.on("/ncsi.txt", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->redirect("http://192.168.4.1/");
  });
  _server.on("/redirect", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->redirect("http://192.168.4.1/");
  });

  // 404 — en mode AP, rediriger vers la page principale (captive portal)
  _server.onNotFound([this](AsyncWebServerRequest* request) {
    if (_wirelessManager && _wirelessManager->getWifiMidi().isAPMode()) {
      request->redirect("http://192.168.4.1/");
    } else {
      request->send(404, "text/plain", "Not found");
    }
  });
}

void WebConfigurator::handleRoot(AsyncWebServerRequest* request) {
  // beginResponse() (not the deprecated _P variant) handles PROGMEM buffers in
  // the maintained ESP32Async fork.
  AsyncWebServerResponse* response = request->beginResponse(200, "text/html", (const uint8_t*)WEB_HTML_CONTENT, sizeof(WEB_HTML_CONTENT) - 1);
  request->send(response);
}

void WebConfigurator::handleApiStatus(AsyncWebServerRequest* request) {
  JsonDocument doc;
  doc["mode"] = _wirelessManager ? _wirelessManager->getStatusText() : "N/A";
  doc["connected"] = _wirelessManager ? _wirelessManager->isMidiConnected() : false;
  doc["uptime"] = millis() / 1000;
  {
    char fw[16];
    snprintf(fw, sizeof(fw), "%d.%d.%d", FIRMWARE_VERSION_MAJOR, FIRMWARE_VERSION_MINOR,
             FIRMWARE_VERSION_PATCH);
    doc["firmware"] = fw;
  }
  // General-Midi-Boop recognition state (additive keys; see docs/GMB_PROTOCOL.md).
  {
    JsonObject g = doc["gmb"].to<JsonObject>();
    char id[11];
    snprintf(id, sizeof(id), "0x%08lX", (unsigned long)gmb::runtime::instanceId());
    g["instance_id"] = id;
    g["revision"] = gmb::runtime::revision();
    g["descriptor_size"] = gmb::runtime::service().descriptorSize();
    g["configured"] = gmb::runtime::isConfigured();
    g["flags"] = gmb::runtime::service().handshakeFlags();
  }

  if (_instrument) {
    doc["cc7"] = _instrument->getCCVolume();
    doc["cc11"] = _instrument->getCCExpression();
    doc["cc1"] = _instrument->getCCModulation();
    doc["cc2"] = _instrument->getCCBreath();
  }

  if (_player) {
    JsonObject player = doc["player"].to<JsonObject>();
    player["state"] = _player->getState();
    player["loaded"] = _player->isFileLoaded();
    player["file"] = _player->getFileName();
    player["events"] = _player->getEventCount();
    player["duration"] = _player->getDurationMs();
    player["position"] = _player->getPositionMs();
    player["progress"] = _player->getProgressPercent();
  }

  String json;
  serializeJson(doc, json);
  request->send(200, "application/json", json);
}

void WebConfigurator::handleApiConfig(AsyncWebServerRequest* request) {
  // Lecteur volumineux execute sur la tache AsyncTCP pendant que loop() peut
  // commiter une nouvelle configuration. Sans ce verrou, la reponse pouvait
  // melanger l'ancienne et la nouvelle config (numNotes deja mis a jour mais
  // notes[] pas encore recopie => lecture au-dela des notes valides).
  // Cette fonction ne fait AUCUN hand-off vers loop() : tenir le verrou ici ne
  // peut pas provoquer d'interblocage.
  if (!lockConfig(WEB_CONFIG_LOCK_MS)) {
    request->send(503, "application/json", "{\"ok\":false,\"error\":\"config_busy\"}");
    return;
  }
  String json = "{";

  // Instrument info
  json += "\"num_fingers\":" + String(cfg.numFingers);
  json += ",\"num_notes\":" + String(cfg.numNotes);
  json += ",\"air_pca\":" + String(cfg.airflowPcaChannel);
  json += ",\"angle_open\":" + String(cfg.fingerAngleOpen);
  json += ",\"half_hole_pct\":" + String(cfg.halfHolePercent);
  json += ",\"embouchure\":" + jsonStr(cfg.embouchure);

  // Scalaires
  json += ",\"midi_ch\":" + String(cfg.midiChannel);
  json += ",\"smidi_on\":" + String(cfg.serialMidiEnabled ? "true" : "false");
  json += ",\"smidi_rx\":" + String(cfg.serialMidiRxPin);
  json += ",\"servo_delay\":" + String(cfg.servoToSolenoidDelayMs);
  json += ",\"valve_interval\":" + String(cfg.minNoteIntervalForValveCloseMs);
  json += ",\"min_note_dur\":" + String(cfg.minNoteDurationMs);
  json += ",\"air_off\":" + String(cfg.servoAirflowOff);
  json += ",\"air_min\":" + String(cfg.servoAirflowMin);
  json += ",\"air_max\":" + String(cfg.servoAirflowMax);
  json += ",\"ang_off\":" + String(cfg.servoAngleOff);
  json += ",\"ang_min\":" + String(cfg.servoAngleMin);
  json += ",\"ang_max\":" + String(cfg.servoAngleMax);
  json += ",\"vib_freq\":" + String(cfg.vibratoFrequencyHz, 1);
  json += ",\"vib_amp\":" + String(cfg.vibratoMaxAmplitudeDeg, 1);
  json += ",\"cc_vol\":" + String(cfg.ccVolumeDefault);
  json += ",\"cc_expr\":" + String(cfg.ccExpressionDefault);
  json += ",\"cc_mod\":" + String(cfg.ccModulationDefault);
  json += ",\"cc_breath\":" + String(cfg.ccBreathDefault);
  json += ",\"cc_bright\":" + String(cfg.ccBrightnessDefault);
  json += ",\"cc2_on\":" + String(cfg.cc2Enabled ? "true" : "false");
  json += ",\"cc2_thr\":" + String(cfg.cc2SilenceThreshold);
  json += ",\"cc2_curve\":" + String(cfg.cc2ResponseCurve, 2);
  json += ",\"cc2_timeout\":" + String(cfg.cc2TimeoutMs);
  json += ",\"sol_act\":" + String(cfg.solenoidPwmActivation);
  json += ",\"sol_hold\":" + String(cfg.solenoidPwmHolding);
  json += ",\"sol_time\":" + String(cfg.solenoidActivationTimeMs);
  json += ",\"device\":" + jsonStr(cfg.deviceName);
  json += ",\"wifi_ssid\":" + jsonStr(cfg.wifiSsid);
  json += ",\"time_unpower\":" + String(cfg.timeUnpower);
  json += ",\"hide_calib\":" + String(cfg.hideCalibration ? "true" : "false");
  json += ",\"hide_air\":" + String(cfg.hideAir ? "true" : "false");
  json += ",\"sol_pin\":" + String(cfg.solenoidPin);
  json += ",\"kbd_mode\":" + String(cfg.kbdMode);
  json += ",\"color\":" + jsonStr(cfg.instrumentColor);
  json += ",\"air_atk_mode\":" + String(cfg.airAttackMode);
  json += ",\"air_atk_off\":" + String(cfg.airAttackOffset);
  json += ",\"air_atk_ms\":" + String(cfg.airAttackMs);
  json += ",\"air_vel_resp\":" + String(cfg.airVelocityResponse);

  // Air delivery system (modulaire)
  json += ",\"air_mode\":" + String(cfg.airMode);
  json += ",\"valve_type\":" + String(cfg.valveType);
  json += ",\"valve_ch\":" + String(cfg.valveServoPcaChannel);
  json += ",\"vlv_close\":" + String(cfg.valveServoCloseAngle);
  json += ",\"vlv_open\":" + String(cfg.valveServoOpenAngle);
  json += ",\"motor_type\":" + String(cfg.motorType);
  json += ",\"fan_pin\":" + String(cfg.fanPin);
  json += ",\"fan_min\":" + String(cfg.fanMinPwm);
  json += ",\"fan_max\":" + String(cfg.fanMaxPwm);
  json += ",\"fan_idle_pct\":" + String(cfg.fanIdlePercent);
  json += ",\"fan_idle_timeout\":" + String(cfg.fanIdleTimeoutMs);
  json += ",\"fan_default_pct\":" + String(cfg.fanDefaultPercent);
  json += ",\"fan_note_max_pct\":" + String(cfg.fanMaxNotePercent);
  json += ",\"fan_follow_air\":" + String(cfg.fanFollowAirflow ? "true" : "false");
  json += ",\"num_pumps\":" + String(cfg.numPumps);
  json += ",\"pump_pins\":[";
  for (int i = 0; i < MAX_PUMPS; i++) {
    if (i > 0) json += ",";
    json += String(cfg.pumpPins[i]);
  }
  json += "],\"pump_mins\":[";
  for (int i = 0; i < MAX_PUMPS; i++) {
    if (i > 0) json += ",";
    json += String(cfg.pumpMinPwm[i]);
  }
  json += "],\"pump_maxs\":[";
  for (int i = 0; i < MAX_PUMPS; i++) {
    if (i > 0) json += ",";
    json += String(cfg.pumpMaxPwm[i]);
  }
  json += "]";
  json += ",\"pump_cascade\":" + String(cfg.pumpCascadeThreshold);
  json += ",\"pump_stagger\":" + String(cfg.pumpStaggerMs);
  json += ",\"pump_idle_pct\":" + String(cfg.pumpDirectIdlePercent);
  json += ",\"pump_direct_max_pct\":" + String(cfg.pumpDirectMaxPercent);
  json += ",\"pump_follow_air\":" + String(cfg.pumpFollowAirflow ? "true" : "false");
  json += ",\"res_target_pct\":" + String(cfg.reservoirTargetPercent);
  json += ",\"res_autostart\":" + String(cfg.reservoirAutoStart ? "true" : "false");
  json += ",\"bb_hyst\":" + String(cfg.bangbangHysteresis);
  json += ",\"sens_type\":" + String(cfg.sensorType);
  json += ",\"sens_target\":" + String(cfg.sensorTargetMm);
  json += ",\"sens_min\":" + String(cfg.sensorMinMm);
  json += ",\"sens_max\":" + String(cfg.sensorMaxMm);
  json += ",\"pid_kp\":" + String(cfg.pidKp);
  json += ",\"pid_ki\":" + String(cfg.pidKi);
  json += ",\"endstop_pin\":" + String(cfg.endstopPin);
  json += ",\"endstop_high\":" + String(cfg.endstopActiveHigh ? "true" : "false");
  json += ",\"endstop_pump_on\":" + String(cfg.endstopPumpOn ? "true" : "false");
  json += ",\"hall_pin\":" + String(cfg.hallPin);
  json += ",\"hall_low\":" + String(cfg.hallThresholdLow);
  json += ",\"hall_high\":" + String(cfg.hallThresholdHigh);
  json += ",\"angle_on\":" + String(cfg.angleServoEnabled ? "true" : "false");
  json += ",\"angle_ch\":" + String(cfg.angleServoPcaChannel);
  json += ",\"show_air\":" + String(cfg.showAirSystem ? "true" : "false");
  json += ",\"res_format\":" + jsonStr(cfg.resFormat);
  json += ",\"midi_limit\":" + String(cfg.midiStorageLimitKb);

#if MIC_ENABLED
  json += ",\"mic\":" + String((_audio && _audio->isMicDetected()) ? "true" : "false");
  json += ",\"mic_status\":\"" + String(_audio ? _audio->getMicStatusString() : "not_init") + "\"";
#else
  json += ",\"mic\":false";
  json += ",\"mic_status\":\"disabled\"";
#endif

  // First boot flag
  json += ",\"first_boot\":" + String(ConfigStorage::isFirstBoot() ? "true" : "false");

  // Doigts (depuis RuntimeConfig)
  json += ",\"fingers\":[";
  for (int i = 0; i < cfg.numFingers; i++) {
    if (i > 0) json += ",";
    json += "{\"ch\":" + String(cfg.fingers[i].pcaChannel);
    json += ",\"a\":" + String(cfg.fingers[i].closedAngle);
    json += ",\"d\":" + String(cfg.fingers[i].direction);
    json += ",\"th\":" + String(cfg.fingers[i].isThumbHole ? 1 : 0);
    if (cfg.fingers[i].halfPercent > 0) {
      json += ",\"hp\":" + String(cfg.fingers[i].halfPercent);
    }
    json += "}";
  }
  json += "]";

  // Notes jouables (complete: MIDI + doigtes + airflow)
  json += ",\"notes\":[";
  for (int i = 0; i < cfg.numNotes; i++) {
    if (i > 0) json += ",";
    json += "{\"midi\":" + String(cfg.notes[i].midiNote);
    json += ",\"amn\":" + String(cfg.notes[i].airflowMinPercent);
    json += ",\"amx\":" + String(cfg.notes[i].airflowMaxPercent);
    json += ",\"anm\":" + String(cfg.notes[i].airflowNominalPercent);
    json += ",\"ang\":" + String(cfg.notes[i].anglePercent);
    json += ",\"fp\":[";
    for (int f = 0; f < cfg.numFingers; f++) {
      if (f > 0) json += ",";
      json += String((int)cfg.notes[i].fingerPattern[f]);
    }
    json += "]}";
  }
  json += "]";

  json += "}";
  unlockConfig();
  request->send(200, "application/json", json);
}

void WebConfigurator::handleApiConfigFinalize(AsyncWebServerRequest* request) {
  // Take ownership of this request's own body (never a shared buffer).
  String configBody; bool configBodyTooLarge;
  takeRequestBody(request, configBody, configBodyTooLarge);

  // A restart is already scheduled: refuse further changes so they cannot overwrite
  // the persisted pending configuration before the reboot applies it.
  if (restartPending()) {
    request->send(409, "application/json", "{\"ok\":false,\"error\":\"restart_pending\"}");
    return;
  }
#if MIC_ENABLED
  // Configuration changes are locked while a calibration owns the actuators, so
  // notes/fingering/air-system/PCA channels cannot shift mid-calibration.
  if (rejectIfCalibrationActive(request)) {
    return;
  }
#endif
  if (configBodyTooLarge) {
    request->send(413, "application/json", "{\"ok\":false,\"msg\":\"Config body too large\"}");
    return;
  }
  if (configBody.length() == 0) {
    request->send(400, "application/json", "{\"ok\":false,\"msg\":\"Empty body\"}");
    return;
  }

  {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, configBody);

    if (err) {
      request->send(400, "application/json", "{\"ok\":false,\"msg\":\"Invalid JSON\"}");
      return;
    }

    // TRANSACTION : on part d'une COPIE de la configuration active. Tout le JSON
    // est applique sur ce candidat, qui est ensuite normalise, valide puis
    // sauvegarde. La configuration active n'est remplacee qu'en cas de succes
    // complet, en une seule affectation faite par la tache loop(). Les
    // controleurs ne peuvent donc jamais observer un etat intermediaire.
    // NB: le candidat est alloue sur le tas car la pile de la tache AsyncTCP est
    // trop etroite pour un RuntimeConfig complet (notes + doigts).
    // `std::nothrow` : le test de nullite ci-dessous etait MORT. Un `new`
    // ordinaire ne rend jamais nullptr - il leve, ou, exceptions desactivees
    // comme ici, il abandonne et la carte redemarre. La protection etait ecrite,
    // relue, et ne pouvait pas se declencher : une requete web sur un tas serre
    // rebootait l'instrument au lieu de recevoir le 500 prevu juste en dessous.
    RuntimeConfig* candidatePtr = new (std::nothrow) RuntimeConfig(cfg);
    if (candidatePtr == nullptr) {
      request->send(500, "application/json", "{\"ok\":false,\"error\":\"out_of_memory\"}");
      return;
    }
    RuntimeConfig& candidate = *candidatePtr;

    // --- Instrument modulaire ---
    if (doc.containsKey("num_fingers")) {
      uint8_t nf = doc["num_fingers"];
      if (nf >= 1 && nf <= MAX_FINGER_SERVOS) candidate.numFingers = nf;
    }
    if (doc.containsKey("air_pca")) candidate.airflowPcaChannel = doc["air_pca"];
    if (doc.containsKey("angle_open")) candidate.fingerAngleOpen = doc["angle_open"];
    if (doc.containsKey("half_hole_pct")) candidate.halfHolePercent = doc["half_hole_pct"];
    if (doc.containsKey("embouchure")) {
      strncpy(candidate.embouchure, doc["embouchure"] | "trav", sizeof(candidate.embouchure) - 1);
      candidate.embouchure[sizeof(candidate.embouchure) - 1] = '\0';
    }

    // --- Scalaires ---
    if (doc.containsKey("midi_ch")) candidate.midiChannel = doc["midi_ch"];
    if (doc.containsKey("smidi_on")) candidate.serialMidiEnabled = doc["smidi_on"].as<bool>();
    if (doc.containsKey("smidi_rx")) {
      uint8_t pin = doc["smidi_rx"];
      // Valider que le GPIO est dans la liste autorisee
      const uint8_t validPins[] = {16,17,18,19,23,25,26,27,33,34,35,36,39};
      for (uint8_t i = 0; i < sizeof(validPins); i++) {
        if (pin == validPins[i]) { candidate.serialMidiRxPin = pin; break; }
      }
    }
    if (doc.containsKey("servo_delay")) candidate.servoToSolenoidDelayMs = doc["servo_delay"];
    if (doc.containsKey("valve_interval")) candidate.minNoteIntervalForValveCloseMs = doc["valve_interval"];
    if (doc.containsKey("min_note_dur")) candidate.minNoteDurationMs = doc["min_note_dur"];
    if (doc.containsKey("air_off")) candidate.servoAirflowOff = doc["air_off"];
    if (doc.containsKey("air_min")) candidate.servoAirflowMin = doc["air_min"];
    if (doc.containsKey("air_max")) candidate.servoAirflowMax = doc["air_max"];
    if (doc.containsKey("ang_pca")) candidate.angleServoPcaChannel = doc["ang_pca"];  // legacy migration
    if (doc.containsKey("ang_off")) candidate.servoAngleOff = doc["ang_off"];
    if (doc.containsKey("ang_min")) candidate.servoAngleMin = doc["ang_min"];
    if (doc.containsKey("ang_max")) candidate.servoAngleMax = doc["ang_max"];
    if (doc.containsKey("vib_freq")) candidate.vibratoFrequencyHz = doc["vib_freq"];
    if (doc.containsKey("vib_amp")) candidate.vibratoMaxAmplitudeDeg = doc["vib_amp"];
    if (doc.containsKey("cc_vol")) candidate.ccVolumeDefault = doc["cc_vol"];
    if (doc.containsKey("cc_expr")) candidate.ccExpressionDefault = doc["cc_expr"];
    if (doc.containsKey("cc_mod")) candidate.ccModulationDefault = doc["cc_mod"];
    if (doc.containsKey("cc_breath")) candidate.ccBreathDefault = doc["cc_breath"];
    if (doc.containsKey("cc_bright")) candidate.ccBrightnessDefault = doc["cc_bright"];
    if (doc.containsKey("cc2_on")) candidate.cc2Enabled = doc["cc2_on"].as<bool>();
    if (doc.containsKey("cc2_thr")) candidate.cc2SilenceThreshold = doc["cc2_thr"];
    if (doc.containsKey("cc2_curve")) candidate.cc2ResponseCurve = doc["cc2_curve"];
    if (doc.containsKey("cc2_timeout")) candidate.cc2TimeoutMs = doc["cc2_timeout"];
    if (doc.containsKey("sol_act")) candidate.solenoidPwmActivation = doc["sol_act"];
    if (doc.containsKey("sol_hold")) candidate.solenoidPwmHolding = doc["sol_hold"];
    if (doc.containsKey("sol_time")) candidate.solenoidActivationTimeMs = doc["sol_time"];
    if (doc.containsKey("time_unpower")) candidate.timeUnpower = doc["time_unpower"];
    if (doc.containsKey("hide_calib")) candidate.hideCalibration = doc["hide_calib"].as<bool>();
    if (doc.containsKey("hide_air")) candidate.hideAir = doc["hide_air"].as<bool>();
    if (doc.containsKey("sol_pin")) candidate.solenoidPin = doc["sol_pin"];
    if (doc.containsKey("kbd_mode")) candidate.kbdMode = doc["kbd_mode"];
    if (doc.containsKey("color")) {
      strncpy(candidate.instrumentColor, doc["color"] | "#D4B044", sizeof(candidate.instrumentColor) - 1);
      candidate.instrumentColor[sizeof(candidate.instrumentColor) - 1] = '\0';
    }
    if (doc.containsKey("air_atk_mode")) candidate.airAttackMode = doc["air_atk_mode"];
    if (doc.containsKey("air_atk_off")) candidate.airAttackOffset = doc["air_atk_off"];
    if (doc.containsKey("air_atk_ms")) candidate.airAttackMs = doc["air_atk_ms"];
    if (doc.containsKey("air_vel_resp")) candidate.airVelocityResponse = doc["air_vel_resp"];

    // Air delivery system (modulaire)
    if (doc.containsKey("air_mode")) {
      uint8_t am = doc["air_mode"];
      // Retro-compat: ancien mode 6 -> mode 5 + endstop meca
      if (am == 6) { am = AIR_MODE_PUMP_RESERVOIR; candidate.sensorType = SENSOR_TYPE_ENDSTOP_MECH; }
      candidate.airMode = am;
    }
    if (doc.containsKey("valve_type")) candidate.valveType = doc["valve_type"];
    // Retro-compat: ancien champ valve_servo
    if (doc.containsKey("valve_servo") && !doc.containsKey("valve_type")) {
      candidate.valveType = doc["valve_servo"].as<bool>() ? 1 : 0;
    }
    if (doc.containsKey("valve_ch")) candidate.valveServoPcaChannel = doc["valve_ch"];
    if (doc.containsKey("angle_on")) candidate.angleServoEnabled = doc["angle_on"].as<bool>();
    if (doc.containsKey("angle_ch")) candidate.angleServoPcaChannel = doc["angle_ch"];
    if (doc.containsKey("vlv_close")) candidate.valveServoCloseAngle = doc["vlv_close"];
    if (doc.containsKey("vlv_open")) candidate.valveServoOpenAngle = doc["vlv_open"];
    // vlv_dir is intentionally ignored; close/open angles fully define valve direction.
    // Compatibilite ascendante : ancienne cle "sol_inter".
    if (!doc.containsKey("valve_interval") && doc.containsKey("sol_inter")) candidate.minNoteIntervalForValveCloseMs = doc["sol_inter"];
    if (doc.containsKey("motor_type")) candidate.motorType = doc["motor_type"];
    if (doc.containsKey("fan_pin")) candidate.fanPin = doc["fan_pin"];
    if (doc.containsKey("fan_min")) candidate.fanMinPwm = doc["fan_min"];
    if (doc.containsKey("fan_max")) candidate.fanMaxPwm = doc["fan_max"];
    if (doc.containsKey("fan_idle_pct")) candidate.fanIdlePercent = doc["fan_idle_pct"];
    if (doc.containsKey("fan_idle_timeout")) candidate.fanIdleTimeoutMs = doc["fan_idle_timeout"];
    if (doc.containsKey("fan_default_pct")) candidate.fanDefaultPercent = doc["fan_default_pct"];
    if (doc.containsKey("fan_note_max_pct")) candidate.fanMaxNotePercent = doc["fan_note_max_pct"];
    if (doc.containsKey("fan_follow_air")) candidate.fanFollowAirflow = doc["fan_follow_air"].as<bool>();
    if (doc.containsKey("num_pumps")) {
      uint8_t np = doc["num_pumps"];
      if (np >= 1 && np <= MAX_PUMPS) candidate.numPumps = np;
    }
    if (doc.containsKey("pump_pins")) {
      JsonArray pp = doc["pump_pins"];
      for (int i = 0; i < MAX_PUMPS && i < (int)pp.size(); i++) candidate.pumpPins[i] = pp[i];
    }
    if (doc.containsKey("pump_mins")) {
      JsonArray pm = doc["pump_mins"];
      for (int i = 0; i < MAX_PUMPS && i < (int)pm.size(); i++) candidate.pumpMinPwm[i] = pm[i];
    }
    if (doc.containsKey("pump_maxs")) {
      JsonArray px = doc["pump_maxs"];
      for (int i = 0; i < MAX_PUMPS && i < (int)px.size(); i++) candidate.pumpMaxPwm[i] = px[i];
    }
    // Retro-compat: ancien champ pump_pin unique
    if (doc.containsKey("pump_pin") && !doc.containsKey("pump_pins")) {
      candidate.pumpPins[0] = doc["pump_pin"];
    }
    if (doc.containsKey("pump_min") && !doc.containsKey("pump_mins")) {
      candidate.pumpMinPwm[0] = doc["pump_min"];
    }
    if (doc.containsKey("pump_max") && !doc.containsKey("pump_maxs")) {
      candidate.pumpMaxPwm[0] = doc["pump_max"];
    }
    if (doc.containsKey("pump_cascade")) {
      uint8_t v = doc["pump_cascade"];
      candidate.pumpCascadeThreshold = (v <= 100) ? v : 100;
    }
    if (doc.containsKey("pump_stagger")) candidate.pumpStaggerMs = doc["pump_stagger"];
    if (doc.containsKey("pump_idle_pct")) candidate.pumpDirectIdlePercent = doc["pump_idle_pct"];
    if (doc.containsKey("pump_direct_max_pct")) candidate.pumpDirectMaxPercent = doc["pump_direct_max_pct"];
    if (doc.containsKey("pump_follow_air")) candidate.pumpFollowAirflow = doc["pump_follow_air"].as<bool>();
    if (doc.containsKey("res_target_pct")) candidate.reservoirTargetPercent = doc["res_target_pct"];
    if (doc.containsKey("res_autostart")) candidate.reservoirAutoStart = doc["res_autostart"].as<bool>();
    if (doc.containsKey("bb_hyst")) {
      uint8_t v = doc["bb_hyst"];
      candidate.bangbangHysteresis = (v <= 50) ? v : 50;
    }
    if (doc.containsKey("sens_type")) candidate.sensorType = doc["sens_type"];
    if (doc.containsKey("sens_target")) candidate.sensorTargetMm = doc["sens_target"];
    if (doc.containsKey("sens_min")) candidate.sensorMinMm = doc["sens_min"];
    if (doc.containsKey("sens_max")) candidate.sensorMaxMm = doc["sens_max"];
    if (doc.containsKey("pid_kp")) candidate.pidKp = doc["pid_kp"];
    if (doc.containsKey("pid_ki")) candidate.pidKi = doc["pid_ki"];
    if (doc.containsKey("endstop_pin")) candidate.endstopPin = doc["endstop_pin"];
    if (doc.containsKey("endstop_high")) candidate.endstopActiveHigh = doc["endstop_high"].as<bool>();
    if (doc.containsKey("endstop_pump_on")) candidate.endstopPumpOn = doc["endstop_pump_on"].as<bool>();
    if (doc.containsKey("hall_pin")) candidate.hallPin = doc["hall_pin"];
    if (doc.containsKey("hall_low")) candidate.hallThresholdLow = doc["hall_low"];
    if (doc.containsKey("hall_high")) candidate.hallThresholdHigh = doc["hall_high"];
    if (doc.containsKey("show_air")) candidate.showAirSystem = doc["show_air"].as<bool>();
    if (doc.containsKey("res_format")) {
      const char* rf = doc["res_format"];
      if (rf) strlcpy(candidate.resFormat, rf, sizeof(candidate.resFormat));
    }
    if (doc.containsKey("midi_limit")) {
      uint16_t ml = doc["midi_limit"];
      if (ml >= 50 && ml <= 2000) candidate.midiStorageLimitKb = ml;
    }

    if (doc.containsKey("device")) {
      strncpy(candidate.deviceName, doc["device"] | candidate.deviceName, sizeof(candidate.deviceName) - 1);
      candidate.deviceName[sizeof(candidate.deviceName) - 1] = '\0';
    }
    if (doc.containsKey("wifi_ssid")) {
      strncpy(candidate.wifiSsid, doc["wifi_ssid"] | "", sizeof(candidate.wifiSsid) - 1);
      candidate.wifiSsid[sizeof(candidate.wifiSsid) - 1] = '\0';
    }
    if (doc.containsKey("wifi_pass")) {
      strncpy(candidate.wifiPassword, doc["wifi_pass"] | "", sizeof(candidate.wifiPassword) - 1);
      candidate.wifiPassword[sizeof(candidate.wifiPassword) - 1] = '\0';
    }

    // --- Doigts (partiel) ---
    if (doc.containsKey("fingers")) {
      JsonArray fingers = doc["fingers"];
      for (int i = 0; i < candidate.numFingers && i < (int)fingers.size(); i++) {
        if (fingers[i].containsKey("ch")) candidate.fingers[i].pcaChannel = fingers[i]["ch"];
        if (fingers[i].containsKey("a")) candidate.fingers[i].closedAngle = fingers[i]["a"];
        if (fingers[i].containsKey("d")) candidate.fingers[i].direction = fingers[i]["d"];
        if (fingers[i].containsKey("th")) candidate.fingers[i].isThumbHole = (fingers[i]["th"].as<int>() != 0);
        if (fingers[i].containsKey("hp")) candidate.fingers[i].halfPercent = constrain(fingers[i]["hp"].as<int>(), 0, 100);
      }
    }

    // --- Notes (complete: remplace tout si present) ---
    if (doc.containsKey("notes")) {
      JsonArray notes = doc["notes"];
      int count = min((int)notes.size(), (int)MAX_NOTES);
      candidate.numNotes = count;
      for (int i = 0; i < count; i++) {
        JsonObject n = notes[i];
        candidate.notes[i].midiNote = n["midi"] | candidate.notes[i].midiNote;
        candidate.notes[i].airflowMinPercent = n["amn"] | candidate.notes[i].airflowMinPercent;
        candidate.notes[i].airflowMaxPercent = n["amx"] | candidate.notes[i].airflowMaxPercent;
        // Nominal: use the provided value, else derive it from min/max so older
        // clients (and stale values) never violate min <= nominal <= max.
        if (n.containsKey("anm")) {
          candidate.notes[i].airflowNominalPercent = n["anm"];
        } else {
          uint8_t mn = candidate.notes[i].airflowMinPercent, mx = candidate.notes[i].airflowMaxPercent;
          candidate.notes[i].airflowNominalPercent = (mx >= mn) ? (uint8_t)(mn + (2 * (mx - mn)) / 5) : mn;
        }
        candidate.notes[i].anglePercent = n["ang"] | candidate.notes[i].anglePercent;
        if (n.containsKey("fp")) {
          JsonArray fp = n["fp"];
          for (int f = 0; f < MAX_FINGER_SERVOS; f++) {
            candidate.notes[i].fingerPattern[f] = (f < (int)fp.size()) ? (uint8_t)fp[f].as<int>() : 0;
          }
        }
      }
    }

    // --- Notes airflow only (backward compat / step 3 save) ---
    if (doc.containsKey("notes_air")) {
      JsonArray notes = doc["notes_air"];
      for (int i = 0; i < candidate.numNotes && i < (int)notes.size(); i++) {
        if (notes[i].containsKey("amn")) candidate.notes[i].airflowMinPercent = notes[i]["amn"];
        if (notes[i].containsKey("amx")) candidate.notes[i].airflowMaxPercent = notes[i]["amx"];
        if (notes[i].containsKey("anm")) {
          candidate.notes[i].airflowNominalPercent = notes[i]["anm"];
        } else if (notes[i].containsKey("amn") || notes[i].containsKey("amx")) {
          // Recompute nominal when the range changed but no explicit nominal was sent.
          uint8_t mn = candidate.notes[i].airflowMinPercent, mx = candidate.notes[i].airflowMaxPercent;
          candidate.notes[i].airflowNominalPercent = (mx >= mn) ? (uint8_t)(mn + (2 * (mx - mn)) / 5) : mn;
        }
        if (notes[i].containsKey("ang")) candidate.notes[i].anglePercent = notes[i]["ang"];
      }
    }

    // --- Notes angle only (step 3 partial save, trav) ---
    if (doc.containsKey("notes_ang")) {
      JsonArray notes = doc["notes_ang"];
      for (int i = 0; i < candidate.numNotes && i < (int)notes.size(); i++) {
        if (notes[i].containsKey("ang")) candidate.notes[i].anglePercent = notes[i]["ang"];
      }
    }

    // Le commit (validation -> sauvegarde -> activation atomique) s'execute sur la
    // tache loop(), proprietaire de `cfg` et des controleurs. Rien n'est ecrit ici.
    WebOp op;
    op.type = WEBOP_COMMIT_CONFIG;
    op.candidate = candidatePtr;
    bool done = runOnLoop(op);
    // runOnLoop() transfere la propriete du candidat a la tache loop() des qu'elle
    // arme l'operation (et remet op.candidate a nullptr). S'il n'a jamais pu
    // l'armer, le candidat revient a l'appelant et doit etre libere ici.
    if (op.candidate) { delete op.candidate; op.candidate = nullptr; }
    if (!done) {
      request->send(503, "application/json", "{\"ok\":false,\"error\":\"busy\"}");
      return;
    }

    if (DEBUG) {
      Serial.println("DEBUG: WebConfigurator - Config commit demande via web");
    }

    request->send(op.httpStatus, "application/json", op.json);
  }
}

void WebConfigurator::handleApiWifiConnect(AsyncWebServerRequest* request) {
  // Single response path, reading this request's own body. Les identifiants sont
  // persistes de facon transactionnelle PUIS la bascule reseau est demandee, le
  // tout sur la tache loop() (voir WEBOP_WIFI_CONNECT).
  String body; bool tooLarge;
  takeRequestBody(request, body, tooLarge);
  if (tooLarge) {
    request->send(413, "application/json", "{\"ok\":false,\"msg\":\"Body too large\"}");
    return;
  }
  if (body.length() == 0) {
    request->send(400, "application/json", "{\"ok\":false,\"msg\":\"Empty body\"}");
    return;
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err || !doc["ssid"].is<const char*>()) {
    request->send(400, "application/json", "{\"ok\":false,\"msg\":\"Invalid JSON\"}");
    return;
  }
  if (!_wirelessManager) {
    request->send(500, "application/json", "{\"ok\":false}");
    return;
  }
  WebOp op;
  op.type = WEBOP_WIFI_CONNECT;
  op.strA = String((const char*)(doc["ssid"] | ""));
  op.strB = String((const char*)(doc["pass"] | ""));
  if (!runOnLoop(op)) {
    request->send(503, "application/json", "{\"ok\":false,\"error\":\"busy\"}");
    return;
  }
  request->send(op.httpStatus, "application/json", op.json);
}

#if MIC_ENABLED
// Nom lisible de la facon dont le suivi d'une note s'est TERMINE. Table pure :
// aucune mesure n'est refaite, c'est une traduction d'enum.
static const char* webTimingOutcomeName(TimingOutcome o) {
  switch (o) {
    case TIMING_OUTCOME_NONE: return "none";
    case TIMING_IN_PROGRESS:  return "in_progress";
    case TIMING_COMPLETE:     return "complete";
    case TIMING_NO_SOUND:     return "no_sound";
    case TIMING_CUT_SHORT:    return "cut_short";
    case TIMING_TIMEOUT:      return "timeout";
    case TIMING_ABORTED:      return "aborted";
    default:                  return "?";
  }
}

// Une duree de chronometrie ne se rend JAMAIS nue : {"valid":bool,"ms":float}.
// Un attackTime jamais mesure vaut 0 ms dans la structure ; publie seul, ce 0
// se lirait comme une attaque instantanee - exactement le defaut deja survenu
// dans ce projet. Le drapeau part donc avec la valeur, systematiquement, par
// cette fonction unique : aucun appelant ne peut l'oublier.
static void webAddTimingMeasure(JsonObject parent, const char* key, const TimingMeasure& m) {
  JsonObject o = parent[key].to<JsonObject>();
  o["valid"] = m.valid;
  o["ms"] = (float)m.ms;
}
#endif  // MIC_ENABLED

void WebConfigurator::handleApiDiagnostics(AsyncWebServerRequest* request) {
  // Diagnostic PUREMENT PASSIF : aucune commande ci-dessous ne fait bouger un
  // actionneur. Un test actif se demande explicitement par les commandes
  // WebSocket de test, qui sont soumises a la protection hardware_not_ready.
  // Meme raison que pour GET /api/config : la copie et les lectures de `cfg`
  // sont serialisees avec le commit execute par loop(). Aucun hand-off vers
  // loop() ici non plus, donc aucun risque d'interblocage.
  if (!lockConfig(WEB_CONFIG_LOCK_MS)) {
    request->send(503, "application/json", "{\"ok\":false,\"error\":\"config_busy\"}");
    return;
  }
  RuntimeConfig tmp = cfg;
  ConfigValidationResult validation = validateAndNormalizeConfig(tmp, &cfg);

  JsonDocument doc;
  JsonArray checks = doc["checks"].to<JsonArray>();
  auto addCheck = [&checks](const char* id, const char* status, const String& message) {
    JsonObject c = checks.add<JsonObject>();
    c["id"] = id;
    c["status"] = status;
    c["message"] = message;
  };

  // --- Etat hardware REEL (plus de "probe requires device") -------------------
  const bool ready = hardwareReady();
  doc["hardware_ready"] = ready;
  if (_instrument) {
    const bool probed = _instrument->hardwareProbeDone();
    doc["pca0_detected"] = probed ? _instrument->isPca0Detected() : false;
    doc["pca1_detected"] = probed ? _instrument->isPca1Detected() : false;
    doc["pca1_required"] = _instrument->isSecondBoardRequired();
    doc["hardware_status"] = (int)_instrument->hardwareInitStatus();
    doc["actuator_session_active"] = _instrument->isActuatorSessionActive();
    doc["dropped_commands"] = _instrument->droppedCommandCount();

    if (!probed) {
      addCheck("pca0", "warning", "Hardware probe not run yet");
      addCheck("pca1", "warning", "Hardware probe not run yet");
    } else {
      addCheck("pca0", _instrument->isPca0Detected() ? "ok" : "error",
               _instrument->isPca0Detected() ? "PCA9685 detected at 0x40"
                                             : "PCA9685 NOT detected at 0x40");
      if (_instrument->isSecondBoardRequired()) {
        addCheck("pca1", _instrument->isPca1Detected() ? "ok" : "error",
                 _instrument->isPca1Detected()
                     ? "Second PCA9685 detected at 0x41"
                     : "Second PCA9685 required by configured channels but NOT detected at 0x41");
      } else {
        addCheck("pca1", "ok",
                 _instrument->isPca1Detected() ? "Second PCA9685 present but not required"
                                               : "Second PCA9685 not required");
      }
    }
    addCheck("hardware", ready ? "ok" : "error",
             ready ? "Actuators initialised and enabled"
                   : "Actuators disabled: hardware_not_ready");
  } else {
    doc["pca0_detected"] = false;
    doc["pca1_detected"] = false;
    doc["pca1_required"] = false;
    doc["hardware_status"] = -1;
    doc["actuator_session_active"] = false;
    doc["dropped_commands"] = 0;
    addCheck("pca0", "error", "Instrument not initialised (boot configuration or filesystem unsafe)");
    addCheck("pca1", "error", "Instrument not initialised");
    addCheck("hardware", "error", "Actuators disabled: hardware_not_ready");
  }

  // --- Configuration ---------------------------------------------------------
  String loadMsg;
  switch (ConfigStorage::lastLoadStatus()) {
    case CONFIG_DEFAULTS: loadMsg = "defaults active"; break;
    case CONFIG_LOADED: loadMsg = "configuration loaded"; break;
    case CONFIG_INVALID_FALLBACK: loadMsg = "invalid /config.json, safe defaults active: " + ConfigStorage::lastLoadError(); break;
    case CONFIG_STORAGE_ERROR: loadMsg = "storage/JSON error, safe defaults active: " + ConfigStorage::lastLoadError(); break;
  }
  addCheck("config", validation.valid ? "ok" : "error",
           validation.valid ? "Runtime configuration is valid" : validation.error);
  bool bootConfigBad = ConfigStorage::lastLoadStatus() == CONFIG_INVALID_FALLBACK ||
                       ConfigStorage::lastLoadStatus() == CONFIG_STORAGE_ERROR;
  addCheck("boot_config", bootConfigBad ? "error" : "ok", loadMsg);
  doc["config_load_status"] = (int)ConfigStorage::lastLoadStatus();
  doc["config_load_error"] = ConfigStorage::lastLoadError();

  // --- Systeme de fichiers ---------------------------------------------------
  FilesystemStatus fsStatus = ConfigStorage::filesystemStatus();
  bool fsOk = ConfigStorage::isFilesystemMounted();
  doc["fs_status"] = (int)fsStatus;
  doc["fs_mounted"] = fsOk;
  doc["fs_error"] = ConfigStorage::filesystemError();
  if (fsOk) {
    addCheck("littlefs", "ok",
             String(LittleFS.usedBytes()) + "/" + String(LittleFS.totalBytes()) + " bytes used");
    doc["fs_used"] = (uint32_t)LittleFS.usedBytes();
    doc["fs_total"] = (uint32_t)LittleFS.totalBytes();
    addCheck("config_file", LittleFS.exists(CONFIG_FILE_PATH) ? "ok" : "warning",
             LittleFS.exists(CONFIG_FILE_PATH) ? "Configuration file is present"
                                               : "Using defaults; /config.json is absent");
  } else {
    // Fail-safe : on NE formate PAS automatiquement. Le mode recovery attend une
    // action volontaire (POST /api/fs/format), donc l'etat est signale tel quel.
    addCheck("littlefs", "error",
             "LittleFS not mounted - recovery mode, actuators disabled, no automatic format");
    doc["fs_used"] = 0;
    doc["fs_total"] = 0;
    addCheck("config_file", "error", "Filesystem unavailable");
  }

  // --- Microphone ------------------------------------------------------------
#if MIC_ENABLED
  bool micOk = (_audio && _audio->isMicDetected());
  doc["microphone_detected"] = micOk;
  doc["microphone_status"] = _audio ? _audio->getMicStatusString() : "not_init";
  addCheck("microphone", micOk ? "ok" : "warning",
           micOk ? "Microphone detected" : "Microphone not detected or not initialized");

  // Acquisition audio : compteurs reels de l'anneau. Un microphone "detecte"
  // qui accumule des debordements produit des mesures sans valeur, et cela doit
  // se voir plutot que de se deviner. Niveau en dBFS (pleine echelle NUMERIQUE,
  // jamais du dB SPL : le microphone n'est pas etalonne).
  if (_audio) {
    const AudioCaptureStats& cap = _audio->getCaptureStats();
    JsonObject a = doc["audio"].to<JsonObject>();
    a["frame_size"] = MIC_ANALYSIS_FRAME_SIZE;
    a["hop_size"] = MIC_ANALYSIS_HOP_SIZE;
    a["sample_rate"] = MIC_SAMPLE_RATE;
    a["samples_received"] = cap.samplesReceived;
    a["frames_produced"] = cap.framesProduced;
    a["partial_reads"] = cap.partialReads;
    a["read_errors"] = cap.readErrors;
    a["buffer_overruns"] = cap.bufferOverruns;
    a["buffer_underruns"] = cap.bufferUnderruns;
    a["dropped_samples"] = cap.droppedSamples;
    a["last_frame_ms"] = (uint32_t)cap.lastFrameTimestamp;
    a["rms_dbfs"] = _audio->getRmsDbFS();
    a["peak_dbfs"] = _audio->getPeakDbFS();
    a["clipping"] = _audio->isClipping();
    a["clipping_ratio"] = _audio->getClippingRatio();
    a["dc_offset"] = _audio->getLevel().dcOffset;

    // Modele de bruit : quels etats ont ete caracterises, et contre lequel le
    // rapport signal/bruit courant est calcule. Un profil manquant se voit,
    // plutot que de se deviner a un SNR trop flatteur.
    const NoiseModel& nm = _audio->getNoiseModel();
    JsonObject nz = a["noise"].to<JsonObject>();
    nz["capturing"] = _audio->isCapturingNoise();
    nz["current"] = NoiseModel::profileName(_audio->currentNoiseProfile());
    nz["captured"] = nm.capturedCount();
    JsonArray profs = nz["profiles"].to<JsonArray>();
    for (uint8_t i = 0; i < NOISE_PROFILE_COUNT; i++) {
      const NoiseProfileId id = (NoiseProfileId)i;
      const NoiseProfile& p = nm.profile(id);
      JsonObject o = profs.add<JsonObject>();
      o["id"] = NoiseModel::profileName(id);
      o["valid"] = p.valid;
      if (p.valid) {
        o["frames"] = p.frames;
        o["rms_dbfs"] = p.rmsDbFS;
        o["flatness"] = p.flatness;
        o["peak_hz"] = p.peakHz;
      }
    }
    // COPIE, pas une reference vivante. Cette fonction s'execute sur la tache
    // AsyncTCP pendant que loop() reecrit _features toutes les 16 ms. Relire le
    // membre champ par champ, avec des allocations ArduinoJson entre deux
    // lectures, laissait la paire hnr_db / hnr_is_spectral venir de DEUX frames
    // differentes : un HNR Goertzel publie avec hnr_is_spectral vrai, soit
    // 31,88 dB d'erreur sur l'echelle meme du chiffre.
    //
    // CE QUE CETTE COPIE NE FAIT PAS : elle n'est pas atomique. 96 octets se
    // copient en plusieurs instructions, et rien n'empeche loop() d'ecrire au
    // milieu. Elle RETRECIT la fenetre de quelques millisecondes (le temps de
    // serialiser le document) a quelques microsecondes ; elle ne la ferme pas.
    // La fermer demanderait un verrou ou un double tampon cote AudioAnalyzer,
    // ce qui n'est pas du ressort de la couche web - et un verrou ferait
    // attendre l'acquisition I2S derriere une serialisation JSON.
    const AcousticFeatures feat = _audio->getFeatures();
    a["snr_valid"] = feat.snrValid;
    a["snr_db"] = feat.snrDb;
    a["snr_fallback"] = feat.snrUsedFallback;

    /*------------------------------------------------------------------------
     * Classification, note de qualite et chronometrie (PHASES 6/7)
     *
     * LECTURE SEULE, ET RIEN QUE DE LA LECTURE. Cette fonction s'execute sur la
     * tache AsyncTCP : elle ne declenche aucun DSP, n'appelle aucun `compute*`
     * et ne fait avancer aucune machine d'etat. Tous les verdicts ci-dessous
     * ont ete produits par loop() (AudioAnalyzer::update()), unique ecrivain.
     *
     * On en prend une COPIE locale immediate, comme pour `feat` ci-dessus.
     * Mais, CONTRAIREMENT au `RuntimeConfig tmp = cfg` du debut de cette
     * fonction - qui, lui, est pris sous lockConfig() et est donc reellement
     * coherent -, aucun verrou n'est pris ici : il n'existe pas de mutex audio,
     * et en introduire un ferait attendre la tache loop(), donc l'acquisition
     * I2S, derriere une serialisation JSON.
     *
     * Ces copies ne sont donc PAS atomiques et ne garantissent PAS la coherence
     * des champs entre eux : loop() peut ecrire pendant la copie. Ce qu'elles
     * apportent est mesurable et limite - la fenetre de lecture passe de la
     * duree de serialisation du document (millisecondes) a celle d'un memcpy
     * (microsecondes). C'est une reduction du risque, pas sa suppression, et un
     * champ lu ici peut encore, rarement, ne pas decrire la meme frame que son
     * voisin. Tout ce qui DOIT rester coherent - une valeur et son drapeau -
     * est donc lu depuis la MEME copie, jamais depuis le membre vivant.
     *-----------------------------------------------------------------------*/
    const AcousticClassification cls = _audio->getClassification();
    const QualityScore           qual = _audio->getQualityScore();
    const BreathinessResult      brth = _audio->getBreathiness();

    // Depuis la COPIE `cls`, et non depuis getAcousticStateName(), qui relit le
    // membre vivant _classification : l'etat serait alors lu dans une frame
    // differente de celle du drapeau publie juste en dessous, et un "good"
    // pouvait partir avec classified:false. Meme regle de nommage que
    // getAcousticStateName() - non classe se dit "unclassified", jamais
    // "silence", qui se lirait comme un verdict.
    a["acoustic_state"] = cls.classified ? AcousticQuality::stateName(cls.state)
                                         : "unclassified";
    a["acoustic_classified"] = cls.classified;
    // quality_weight_used accompagne TOUJOURS quality_score. Le score est une
    // moyenne ponderee de sept criteres ; l'attaque (10 % du cahier des
    // charges) peut ne pas avoir ete mesuree, le score est alors renormalise
    // sur les six autres. 0,82 pondere a 1,00 et 0,82 pondere a 0,90 ne
    // decrivent pas le meme son : un score publie sans son poids ment sur ce
    // qu'il mesure.
    a["quality_score"] = qual.score;
    a["quality_weight_used"] = qual.weightUsed;
    a["quality_valid"] = qual.valid;
    // breathiness_weight_used accompagne TOUJOURS breathiness, pour la meme
    // raison que le couple ci-dessus, et avec un ecart PLUS GRAND : la
    // respiration est une moyenne ponderee de trois composantes dont deux
    // (HNR spectral, platitude) n'existent qu'une frame sur
    // MIC_SPECTRAL_DECIMATION. weightUsed descend jusqu'a 0,30 quand celui de
    // la qualite ne descend qu'a 0,75. Sur une note tenue immobile, la valeur
    // alterne entre une mesure pleine et 0,00 a 62,5 Hz avec valid=true dans
    // les deux cas : seul le poids distingue "pas de souffle" de "presque rien
    // de mesure".
    a["breathiness"] = brth.value;
    a["breathiness_weight_used"] = brth.weightUsed;
    a["breathiness_valid"] = brth.valid;
    // Deux echelles distinctes derriere un seul champ : mesure spectrale (FFT)
    // ou approximation Goertzel a quatre raies. Elles ne se comparent pas, donc
    // le chiffre ne part pas sans dire laquelle il est.
    //
    // ET IL NE PART PAS NON PLUS SANS DIRE S'IL A ETE MESURE. Quand
    // spectralValid est faux - aucune note en cours, ou apres tout
    // markMeasurementInvalid() - fillSpectral() remet harmonicToNoiseRatio a
    // 0.0f. Publie tel quel, ce "hnr_db": 0 etait INDISTINGUABLE d'une vraie
    // mesure Goertzel autour de 0 dB - la reference documentee d'une note
    // timbree sur cette echelle est -0,06 dB. Le drapeau dit desormais l'etat,
    // et le chiffre n'est emis que MESURE - meme discipline que la poussee
    // WebSocket, qui omet "hnr" dans ce cas, et que snr_valid / snr_db
    // ci-dessus. Les deux champs partent ensemble ou pas du tout : une echelle
    // sans valeur ne dit rien, une valeur sans echelle ment.
    a["hnr_valid"] = feat.spectralValid;
    if (feat.spectralValid) {
      a["hnr_db"] = feat.harmonicToNoiseRatio;
      a["hnr_is_spectral"] = feat.hnrIsSpectral;
    }

    // De QUOI la classification a ete privee. Un etat "good" obtenu faute
    // d'avoir pu mesurer le pitch, le rapport signal/bruit ou le spectre n'est
    // pas un etat "good" : sans ces drapeaux l'interface ne peut pas faire la
    // difference entre "c'est bon" et "je n'ai rien pu evaluer".
    // `snr_fallback` a la meme fonction : le SNR a ete compare au profil d'un
    // AUTRE etat machine que l'etat reel, il surestime donc probablement la
    // qualite. Il est ici sous sa forme vue par la CLASSIFICATION ; le champ
    // de meme nom au niveau de `a` reste celui de la frame (AcousticFeatures).
    JsonObject missing = a["missing"].to<JsonObject>();
    missing["pitch"] = cls.missingPitch;
    missing["snr"] = cls.missingSnr;
    missing["spectrum"] = cls.missingSpectrum;
    missing["expected_note"] = cls.missingExpectedNote;
    missing["stability"] = cls.missingStability;
    missing["squeak_history"] = cls.missingSqueakHistory;
    missing["snr_fallback"] = cls.snrUsedFallback;

    // Chronometrie de la DERNIERE note terminee (PHASE 7). Elle est ici et pas
    // sur la poussee WebSocket : ce bloc pese a lui seul plus que la poussee
    // entiere, qui part 10 fois par seconde vers jusqu'a WS_MAX_CLIENTS
    // clients. Ici il ne coute que lorsqu'un humain ouvre le diagnostic.
    //
    // `_last` est remis a zero en meme temps que `_hasLast`, donc le lire quand
    // has_last est faux rend des mesures toutes invalides - jamais des restes
    // d'une note precedente.
    const AcousticTiming& tmg = _audio->timing();
    const bool timingHasLast = tmg.hasLast();
    const NoteTiming note = tmg.last();
    JsonObject tm = a["timing"].to<JsonObject>();
    tm["has_last"] = timingHasLast;
    tm["outcome"] = webTimingOutcomeName(note.outcome);
    // Le plancher d'avant-note conditionne le seuil d'apparition : sans lui,
    // toutes les durees qui en decoulent sont des suppositions.
    tm["baseline_valid"] = note.baselineValid;
    // CHAQUE duree porte sa validite. Un "attack":{"ms":0} sans son "valid"
    // se lirait comme une attaque instantanee alors qu'il signifie "jamais
    // mesuree" : c'est le defaut precis a ne pas reintroduire.
    webAddTimingMeasure(tm, "command_to_sound", note.commandToSoundLatency);
    webAddTimingMeasure(tm, "air_to_sound", note.airToSoundLatency);
    webAddTimingMeasure(tm, "attack", note.attackTime);
    webAddTimingMeasure(tm, "pitch_stabilization", note.pitchStabilizationTime);
    webAddTimingMeasure(tm, "release", note.releaseTime);
    // Les DEUX modes de panne de la chronometrie, sans lesquels un releve vide
    // ou immobile ne se distingue pas d'une absence de jeu. Ils n'avaient aucun
    // consommateur : les compter sans jamais les publier revient a ne pas les
    // compter.
    //   rejected_frames : frames refusees parce que leur horodatage RECULE
    //     (le repliement de millis() n'en fait pas partie). La machine a etats
    //     n'avance pas sur ces frames, et les durees qui en dependent ne sont
    //     jamais mesurees.
    //   rejected_events : ordres d'actionneur refuses parce qu'ils arrivent
    //     hors de la fenetre ou ils ont un sens (air ou valve apres que le son
    //     sonne, deuxieme occurrence, evenement anterieur a l'ordre MIDI, arret
    //     sans note en cours). Un compteur qui monte ici designe un cablage
    //     d'appels errone, pas un defaut de jeu.
    // Compteurs cumulatifs 16 bits remis a zero par AcousticTiming::reset().
    tm["rejected_frames"] = tmg.rejectedFrames();
    tm["rejected_events"] = tmg.rejectedEvents();

    if (nm.capturedCount() == 0) {
      addCheck("noise_model", "warning",
               "No noise profile captured: SNR is unavailable");
    } else if (feat.snrUsedFallback) {
      addCheck("noise_model", "warning",
               String("No profile for ") + NoiseModel::profileName(_audio->currentNoiseProfile()) +
                   "; SNR falls back to ambient and likely overstates quality");
    } else {
      addCheck("noise_model", "ok",
               String((unsigned long)nm.capturedCount()) + " noise profile(s) captured");
    }

    // Des echantillons perdus signifient que loop() n'a pas suivi : les mesures
    // portent alors sur un signal troue. C'est un avertissement, pas une panne.
    if (cap.droppedSamples > 0) {
      addCheck("audio_capture", "warning",
               String("Audio capture dropped ") + String((unsigned long)cap.droppedSamples) +
                   " samples (" + String((unsigned long)cap.bufferOverruns) + " overruns)");
    } else if (_audio->isClipping()) {
      addCheck("audio_capture", "warning", "Microphone input is clipping");
    } else {
      addCheck("audio_capture", "ok",
               String((unsigned long)cap.framesProduced) + " frames analysed, no dropped samples");
    }
  }
  doc["calibration_active"] = isCalibrationActive();
#else
  doc["microphone_detected"] = false;
  doc["microphone_status"] = "disabled";
  addCheck("microphone", "warning", "Microphone support disabled at compile time");
  doc["calibration_active"] = false;
#endif

  // --- Capteur de reservoir (etats distincts) --------------------------------
  {
    JsonObject sensor = doc["sensor"].to<JsonObject>();
    sensor["type"] = cfg.sensorType;
    sensor["used"] = configurationUsesReservoirSensor(cfg);
    if (_instrument && configurationUsesReservoirSensor(cfg)) {
      PressureController& pc = _instrument->getPressureCtrl();
      sensor["tof"] = pc.usesTofSensor();
      sensor["present_on_bus"] = pc.usesTofSensor() ? pc.isSensorPresentOnBus() : true;
      sensor["initialized"] = pc.usesTofSensor() ? pc.isSensorInitialized() : pc.isSensorDetected();
      sensor["usable"] = pc.isSensorDetected();
      sensor["measurement_valid"] = pc.isMeasurementValid();
      sensor["measurement_stale"] = pc.isMeasurementStale();
      sensor["state"] = pc.sensorStateName();
      sensor["distance_mm"] = pc.getDistanceMm();
      const char* status = pc.isSensorDetected()
                               ? (pc.isMeasurementStale() ? "warning" : "ok")
                               : "error";
      addCheck("sensor", status, String("Reservoir sensor: ") + pc.sensorStateName());
    } else {
      sensor["tof"] = false;
      sensor["present_on_bus"] = false;
      sensor["initialized"] = false;
      sensor["usable"] = false;
      sensor["measurement_valid"] = false;
      sensor["measurement_stale"] = false;
      sensor["state"] = "not_used";
      addCheck("sensor", "ok", "Reservoir sensor not used by the selected air mode");
    }
  }

  // --- Transports MIDI -------------------------------------------------------
  {
    JsonObject midi = doc["midi"].to<JsonObject>();
    bool wifiMode = _wirelessManager && _wirelessManager->getMode() != MODE_BLUETOOTH;
    midi["mode"] = wifiMode ? "wifi" : "ble";
    midi["ble"] = !wifiMode;
    midi["rtpmidi"] = wifiMode;
    midi["din"] = cfg.serialMidiEnabled;
    midi["connected"] = _wirelessManager ? _wirelessManager->isMidiConnected() : false;
    if (_wirelessManager) midi["status"] = _wirelessManager->getStatusText();
    addCheck("midi_transports", "ok",
             String("Active transport: ") + (wifiMode ? "rtpMIDI + web" : "BLE-MIDI") +
                 (cfg.serialMidiEnabled ? ", MIDI DIN in" : ""));
  }

  // --- General-Midi-Boop -----------------------------------------------------
  {
    JsonObject g = doc["gmb"].to<JsonObject>();
    g["revision"] = gmb::runtime::revision();
    g["configured"] = gmb::runtime::isConfigured();
    g["descriptor_size"] = gmb::runtime::service().descriptorSize();
  }

  // --- Divers ----------------------------------------------------------------
  doc["heap"] = ESP.getFreeHeap();
  doc["heap_min"] = ESP.getMinFreeHeap();
  doc["uptime"] = millis() / 1000;
  doc["restart_pending"] = restartPending();
  doc["upload_active"] = (_upload.owner != nullptr);
  doc["web_sessions"] = _auth.activeSessions(millis());
  addCheck("heap", ESP.getFreeHeap() > 20000 ? "ok" : "warning",
           String(ESP.getFreeHeap()) + " bytes free heap");
  addCheck("restart", restartPending() ? "warning" : "ok",
           restartPending() ? "Controlled restart pending" : "No restart pending");

  doc["ok"] = validation.valid && !bootConfigBad && fsOk && ready;

  unlockConfig();

  String out;
  serializeJson(doc, out);
  request->send(200, "application/json", out);
}

void WebConfigurator::handleApiConfigReset(AsyncWebServerRequest* request) {
#if MIC_ENABLED
  // Never rewrite notes/fingerings/servo ranges while a calibration is using the
  // current organisation.
  if (rejectIfCalibrationActive(request)) return;
#endif
  // A reset rewrites the whole configuration; a reboot is required for the new
  // config to take effect cleanly. Refuse while a reboot is already pending so a
  // second reset cannot race the one in flight.
  if (restartPending()) {
    request->send(409, "application/json", "{\"ok\":false,\"error\":\"restart_pending\"}");
    return;
  }
  WebOp op;
  op.type = WEBOP_RESET_CONFIG;
  if (!runOnLoop(op)) {
    request->send(503, "application/json", "{\"ok\":false,\"error\":\"busy\"}");
    return;
  }
  if (DEBUG) {
    Serial.println("DEBUG: WebConfigurator - Config reset aux defauts");
  }
  request->send(op.httpStatus, "application/json", op.json);
}

void WebConfigurator::handleApiFactoryReset(AsyncWebServerRequest* request) {
#if MIC_ENABLED
  if (rejectIfCalibrationActive(request)) return;
#endif
  if (restartPending()) {
    request->send(409, "application/json", "{\"ok\":false,\"error\":\"restart_pending\"}");
    return;
  }
  WebOp op;
  op.type = WEBOP_FACTORY_RESET;
  if (!runOnLoop(op)) {
    request->send(503, "application/json", "{\"ok\":false,\"error\":\"busy\"}");
    return;
  }
  if (DEBUG) {
    Serial.println("DEBUG: WebConfigurator - Reset usine");
  }
  request->send(op.httpStatus, "application/json", op.json);
}

void WebConfigurator::handleMidiUpload(AsyncWebServerRequest* request, const String& filename,
                                        size_t index, uint8_t* data, size_t len, bool final) {
  if (index == 0) {
    if (DEBUG) {
      Serial.print("DEBUG: WebConfigurator - Upload MIDI: ");
      Serial.println(filename);
    }
    // Verrou EXCLUSIF : les anciens membres partages (_uploadFile, _uploadSize,
    // _uploadFileName, _uploadError) etaient uniques pour tout le serveur, donc
    // deux clients simultanes ecrivaient dans le MEME descripteur, melangeaient
    // leurs octets et se volaient le nom de destination. Le slot d'upload
    // appartient desormais a UNE requete a la fois ; un second client est refuse
    // proprement (409 upload_busy) et ne touche jamais au transfert en cours.
    if (!acquireUploadLock(request)) {
      // Pas de slot : on ne touche a rien, handleMidiUploadComplete() repondra.
      return;
    }
    _upload.size = 0;
    _upload.error = false;
    _upload.errorCode = "";
    _upload.fileName = "";
    _upload.tmpPath = "";

    // Nom de destination assaini (pas de chemin, extension .mid/.midi imposee).
    if (!sanitizeMidiFileName(filename, _upload.fileName)) {
      _upload.error = true;
      _upload.errorCode = "invalid_name";
      return;
    }

    // Fichier temporaire UNIQUE, hors de MIDI_DIR : il n'apparait donc ni dans la
    // liste des fichiers ni dans le calcul d'occupation.
    _upload.tmpPath = String("/.up") + String(++_uploadSequence) + ".tmp";
    if (LittleFS.exists(_upload.tmpPath)) LittleFS.remove(_upload.tmpPath);
    _upload.file = LittleFS.open(_upload.tmpPath, "w");
    if (!_upload.file) {
      if (DEBUG) Serial.println("ERREUR: WebConfigurator - Unable to create temp file");
      _upload.error = true;
      _upload.errorCode = "temp_open_failed";
      return;
    }
  }

  // Un client qui n'a pas le verrou ne doit rien ecrire.
  if (_upload.owner != request || _upload.error) return;
  _upload.lastActivity = millis();

  if (len > 0) {
    if (_upload.size + len > MIDI_FILE_MAX_SIZE) {
      // Refuser des l'octet de trop : inutile d'ecrire un fichier deja rejete.
      _upload.error = true;
      _upload.errorCode = "too_large";
      if (_upload.file) _upload.file.close();
      return;
    }
    size_t written = _upload.file ? _upload.file.write(data, len) : 0;
    if (written != len) {
      // Ecriture partielle = LittleFS plein ou en ereur.
      _upload.error = true;
      _upload.errorCode = "write_failed";
      if (_upload.file) _upload.file.close();
      return;
    }
    _upload.size += len;
  }

  if (final && _upload.file) {
    _upload.file.close();
    if (DEBUG) {
      Serial.print("DEBUG: WebConfigurator - Upload termine: ");
      Serial.print(_upload.size);
      Serial.println(" octets");
    }
  }
}

void WebConfigurator::handleMidiUploadComplete(AsyncWebServerRequest* request) {
  auto respond = [&](int status, const char* code, const char* message) {
    JsonDocument doc;
    doc["ok"] = false;
    doc["error"] = code;
    doc["msg"] = message;
    String out;
    serializeJson(doc, out);
    request->send(status, "application/json", out);
    JsonDocument ws;
    ws["t"] = "midi_error";
    ws["msg"] = message;
    ws["reason"] = code;
    String wsOut;
    serializeJson(ws, wsOut);
    _ws.textAll(wsOut);
  };

  if (_upload.owner != request) {
    // Un autre transfert detenait le verrou : celui-ci n'a rien ecrit.
    respond(409, "upload_busy", "Another upload is in progress");
    return;
  }

  if (_upload.error) {
    const char* code = _upload.errorCode;
    releaseUploadLock(request);
    if (strcmp(code, "unauthorized") == 0) respond(401, code, "Authentication required");
    else if (strcmp(code, "too_large") == 0) respond(413, code, "File too large");
    else if (strcmp(code, "invalid_name") == 0) respond(400, code, "Invalid file name (.mid/.midi expected)");
    else if (strcmp(code, "write_failed") == 0) respond(507, code, "Storage write error (filesystem full?)");
    else respond(500, code, "Temporary file write error");
    return;
  }

  if (_upload.size == 0) {
    releaseUploadLock(request);
    request->send(400, "application/json", "{\"ok\":false,\"error\":\"empty\",\"msg\":\"No file received\"}");
    return;
  }

  // Validation du contenu, quota et remplacement : LittleFS + lecteur MIDI, donc
  // sur la tache loop(). Le verrou n'est relache qu'apres.
  WebOp op;
  op.type = WEBOP_MIDI_FINALIZE;
  bool done = runOnLoop(op);
  releaseUploadLock(request);
  if (!done) {
    request->send(503, "application/json", "{\"ok\":false,\"error\":\"busy\"}");
    return;
  }
  if (!op.ok) {
    JsonDocument ws;
    ws["t"] = "midi_error";
    ws["msg"] = "Upload rejected";
    String wsOut;
    serializeJson(ws, wsOut);
    _ws.textAll(wsOut);
  }
  request->send(op.httpStatus, "application/json", op.json);
}

size_t WebConfigurator::getMidiStorageUsed() {
  size_t total = 0;
  File dir = LittleFS.open(MIDI_DIR);
  if (!dir || !dir.isDirectory()) return 0;
  File f = dir.openNextFile();
  while (f) {
    if (!f.isDirectory()) {
      total += f.size();
    }
    f = dir.openNextFile();
  }
  return total;
}

void WebConfigurator::handleMidiList(AsyncWebServerRequest* request) {
  JsonDocument doc;
  JsonArray files = doc["files"].to<JsonArray>();
  File dir = LittleFS.open(MIDI_DIR);
  if (dir && dir.isDirectory()) {
    File f = dir.openNextFile();
    while (f) {
      if (!f.isDirectory()) {
        String fname = String(f.name());
        int lastSlash = fname.lastIndexOf('/');
        if (lastSlash >= 0) fname = fname.substring(lastSlash + 1);
        JsonObject entry = files.add<JsonObject>();
        entry["name"] = fname;
        entry["size"] = f.size();
      }
      f = dir.openNextFile();
    }
  }
  doc["used"] = getMidiStorageUsed();
  doc["limit"] = (size_t)cfg.midiStorageLimitKb * 1024;
  if (_player && _player->isFileLoaded()) {
    doc["loaded"] = _player->getFileName();
  }
  String json;
  serializeJson(doc, json);
  request->send(200, "application/json", json);
}

void WebConfigurator::handleMidiDelete(AsyncWebServerRequest* request) {
  String body; bool tooLarge;
  takeRequestBody(request, body, tooLarge);
  if (tooLarge) {
    request->send(413, "application/json", "{\"ok\":false,\"msg\":\"Body too large\"}");
    return;
  }
  if (body.length() == 0) {
    request->send(400, "application/json", "{\"ok\":false,\"msg\":\"Empty body\"}");
    return;
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err || !doc["file"].is<const char*>()) {
    request->send(400, "application/json", "{\"ok\":false,\"msg\":\"Invalid JSON\"}");
    return;
  }
  String filename;
  if (!sanitizeMidiFileName(doc["file"].as<String>(), filename)) {
    request->send(400, "application/json", "{\"ok\":false,\"msg\":\"Invalid name\"}");
    return;
  }
  WebOp op;
  op.type = WEBOP_MIDI_DELETE;
  op.strA = filename;
  if (!runOnLoop(op)) {
    request->send(503, "application/json", "{\"ok\":false,\"error\":\"busy\"}");
    return;
  }
  if (DEBUG && op.ok) {
    Serial.print("DEBUG: WebConfigurator - MIDI deleted: ");
    Serial.println(filename);
  }
  request->send(op.httpStatus, "application/json", op.json);
}

void WebConfigurator::handleMidiLoad(AsyncWebServerRequest* request) {
  String body; bool tooLarge;
  takeRequestBody(request, body, tooLarge);
  if (tooLarge) {
    request->send(413, "application/json", "{\"ok\":false,\"msg\":\"Body too large\"}");
    return;
  }
  if (body.length() == 0) {
    request->send(400, "application/json", "{\"ok\":false,\"msg\":\"Empty body\"}");
    return;
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err || !doc["file"].is<const char*>()) {
    request->send(400, "application/json", "{\"ok\":false,\"msg\":\"Invalid JSON\"}");
    return;
  }
  String filename;
  if (!sanitizeMidiFileName(doc["file"].as<String>(), filename)) {
    request->send(400, "application/json", "{\"ok\":false,\"msg\":\"Invalid name\"}");
    return;
  }
  WebOp op;
  op.type = WEBOP_MIDI_LOAD;
  op.strA = filename;
  if (!runOnLoop(op)) {
    request->send(503, "application/json", "{\"ok\":false,\"error\":\"busy\"}");
    return;
  }
  request->send(op.httpStatus, "application/json", op.json);
}

// --- WebSocket ---

void WebConfigurator::onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                                 AwsEventType type, void* arg, uint8_t* data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT:
      // Un nouveau client n'est PAS authentifie : il doit envoyer
      // {"t":"auth","token":"..."} avant toute commande.
      clearWsAuthentication(client->id());
      client->text("{\"t\":\"auth_required\"}");
      if (DEBUG) {
        Serial.print("DEBUG: WS client connected #");
        Serial.println(client->id());
      }
      break;

    case WS_EVT_DISCONNECT: {
      clearWsAuthentication(client->id());
      bool handled = false;
#if MIC_ENABLED
      if (isCalibrationActive()) {
        // Only the owner's disconnect stops the calibration and safes the
        // hardware. A non-owner leaving must NOT disrupt the running session
        // (an allSoundOff would fight the calibrator), so we do nothing.
        handled = true;
        if (client->id() == _autoCalOwnerClientId) {
          // Deconnexion du proprietaire : la calibration doit etre annulee et le
          // materiel remis en securite, mais depuis la tache loop() - et SANS
          // attendre ici, car ce callback peut detenir le verrou du WebSocket.
          // Drapeau non perdable (cf. update()) plutot qu'un postWebOp() qui
          // echouerait silencieusement sur file pleine.
          requestCalibrationCancel();
          if (_instrument) _instrument->requestPanic();
        }
      }
#endif
      if (!handled) {
        // Outside calibration, only the OWNER of an active manual test triggers a
        // safe state on disconnect. A status-only client (or the client that
        // merely started MIDI playback) leaving must not stop the instrument.
        if (_testActive && client->id() == _testOwnerClientId) {
          endTestSession(true);
        }
      }
      if (DEBUG) {
        Serial.print("DEBUG: WS client disconnected #");
        Serial.println(client->id());
      }
      break;
    }

    case WS_EVT_DATA: {
      AwsFrameInfo* info = (AwsFrameInfo*)arg;
      if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
        processWsMessage(client, data, len);
      }
      break;
    }

    default:
      break;
  }
}

void WebConfigurator::processWsMessage(AsyncWebSocketClient* client, uint8_t* data, size_t len) {
  if (len > 512) {
    client->text("{\"t\":\"error\",\"msg\":\"WebSocket message too large\"}");
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, data, len);
  if (err) {
    client->text("{\"t\":\"error\",\"msg\":\"Invalid JSON\"}");
    return;
  }

  const char* type = doc["t"] | "";
  auto hasInt = [&doc](const char* key) { return isJsonInteger(doc[key]); };

  // --- 1. Authentification ---------------------------------------------------
  // Le premier message d'un client doit etre {"t":"auth","token":"..."} ; tant
  // qu'il n'est pas authentifie, aucune commande n'est acceptee.
  if (strcmp(type, "auth") == 0) {
    String token = String((const char*)(doc["token"] | ""));
    bool ok = _auth.validate(token, millis());
    // Le jeton lui-meme est conserve : chaque commande suivante le revalidera.
    if (ok) setWsAuthenticated(client->id(), token);
    else    clearWsAuthentication(client->id());
    client->text(ok ? "{\"t\":\"auth\",\"ok\":true}"
                    : "{\"t\":\"auth\",\"ok\":false,\"msg\":\"unauthorized\"}");
    return;
  }
  if (!isWsAuthenticated(client->id())) {
    client->text("{\"t\":\"error\",\"msg\":\"unauthorized\"}");
    return;
  }

  if (_instrument == nullptr) {
    // Instrument absent (systeme de fichiers ou configuration de boot non sure) :
    // meme reponse explicite que pour un hardware non pret.
    client->text("{\"t\":\"error\",\"msg\":\"hardware_not_ready\"}");
    return;
  }

  // --- 2. Protection hardware_not_ready -------------------------------------
  // Refus CENTRAL de toute commande physique quand l'initialisation hardware a
  // echoue (PCA0/PCA1 absents, configuration de boot invalide). Le refus est
  // double par InstrumentManager::applyCommand(), qui est le seul point
  // d'application : aucun chemin ne peut activer un actionneur.
  if (isPhysicalWsCommand(type) && !hardwareReady()) {
    client->text("{\"t\":\"error\",\"msg\":\"hardware_not_ready\"}");
    return;
  }

#if MIC_ENABLED
  // While a calibration owns the actuators, refuse concurrent actuator commands.
  if (actuatorCommandBlockedDuringCalibration(client, type)) return;
#endif

  // Limitation de debit des commandes qui commandent un mouvement de plus. Sans
  // elle, un client pouvait remplir la file de commandes et saturer le bus I2C
  // aussi vite qu'il ecrivait, alors que les CC MIDI etaient deja limites depuis
  // toujours. Le rejet est SILENCIEUX : repondre a chaque message jete
  // amplifierait le flot au lieu de le contenir.
  if (isRateLimitedWsCommand(type) && !allowActuatorCommand(millis())) {
    return;
  }

  // A manual actuator test starts/refreshes a bounded, owner-tracked session so the
  // server (not just the browser) returns the hardware to safe on loss of contact.
  // If another client already owns an active test, refuse rather than hijack it.
  if (isManualTestCommand(type) && !beginTestSession(client->id())) {
    client->text("{\"t\":\"test_busy\"}");
    return;
  }

  // --- 3. Commandes -----------------------------------------------------------
  // Toutes les commandes qui touchent un actionneur, le bus I2C ou un GPIO sont
  // POSTEES vers la tache loop() (voir CommandQueue.h). Ce callback s'execute sur
  // la tache AsyncTCP et ne doit jamais piloter le materiel directement.
  if (strcmp(type, "non") == 0) {
    if (!hasInt("n")) return;
    uint8_t note = getMidi7Bit(doc, "n", 0);
    uint8_t vel = (uint8_t)constrain(doc["v"] | _webVelocity, 1, MIDI_VELOCITY_MAX);
    _instrument->postCommand(ACMD_NOTE_ON, note, vel);
  } else if (strcmp(type, "nof") == 0) {
    if (!hasInt("n")) return;
    _instrument->postCommand(ACMD_NOTE_OFF, getMidi7Bit(doc, "n", 0));
  } else if (strcmp(type, "cc") == 0) {
    if (!hasInt("c") || !hasInt("v")) return;
    _instrument->postCommand(ACMD_CONTROL_CHANGE, getMidi7Bit(doc, "c", 0), getMidi7Bit(doc, "v", 0));
  } else if (strcmp(type, "velocity") == 0) {
    _webVelocity = (uint8_t)constrain(doc["v"] | _webVelocity, 1, MIDI_VELOCITY_MAX);
  } else if (strcmp(type, "air_live") == 0) {
    _instrument->postCommand(ACMD_AIR_LIVE_PERCENT, 0, getPercent(doc, "v", 0));
  } else if (strcmp(type, "play") == 0) {
    WebOp op; op.type = WEBOP_PLAYER_PLAY; postWebOp(op);
  } else if (strcmp(type, "pause") == 0) {
    WebOp op; op.type = WEBOP_PLAYER_PAUSE; postWebOp(op);
  } else if (strcmp(type, "stop") == 0) {
#if MIC_ENABLED
    // During a calibration, "stop" must cancel it cleanly (its own stop already
    // safes the hardware) rather than fight the calibrator with allSoundOff.
    if (isCalibrationActive()) {
      if (_autoCalOwnerClientId != 0 && client->id() != _autoCalOwnerClientId) {
        client->text("{\"t\":\"error\",\"msg\":\"not_calibration_owner\"}");
      } else {
        requestCalibrationCancel();
      }
      return;
    }
#endif
    WebOp op; op.type = WEBOP_PLAYER_STOP; postWebOp(op);
    _instrument->requestPanic();
  } else if (strcmp(type, "ch_filter") == 0) {
    WebOp op; op.type = WEBOP_PLAYER_CH_FILTER; op.intA = doc["ch"] | 255; postWebOp(op);
  } else if (strcmp(type, "panic") == 0) {
#if MIC_ENABLED
    // Panic must always abort a running calibration and safe the hardware first.
    // Drapeau non perdable : un panic ne doit jamais laisser _autoCal "running".
    requestCalibrationCancel();
#endif
    endTestSession(false);   // hardware is safed just below by the panic request
    // Le panic n'occupe pas une place de la file : il ne peut pas etre perdu et
    // il annule toutes les commandes deja en attente.
    _instrument->requestPanic();
  } else if (strcmp(type, "test_finger") == 0) {
    int fi = doc["i"] | -1;
    int angle = getServoAngle(doc, "a", 0);
    if (fi >= 0 && fi < cfg.numFingers) {
      _instrument->postCommand(ACMD_TEST_FINGER, (uint8_t)fi, 0, (uint16_t)angle);
    }
  } else if (strcmp(type, "test_air") == 0) {
    _instrument->postCommand(ACMD_TEST_AIRFLOW_ANGLE, 0, 0, getServoAngle(doc, "a", 0));
  } else if (strcmp(type, "test_angle") == 0) {
    _instrument->postCommand(ACMD_TEST_ANGLE_SERVO, 0, 0, getServoAngle(doc, "a", 0));
  } else if (strcmp(type, "angle_live") == 0) {
    _instrument->postCommand(ACMD_ANGLE_LIVE_PERCENT, 0, getPercent(doc, "v", 0));
  } else if (strcmp(type, "test_sol") == 0) {
    _instrument->postCommand(ACMD_TEST_SOLENOID, (doc["o"] | 0) != 0 ? 1 : 0);
  } else if (strcmp(type, "test_note") == 0) {
    uint8_t note = getMidi7Bit(doc, "n", 0);
    if (_instrument->isNotePlayable(note)) {
      // Play a REAL, timed note through the sequencer: it positions the fingers,
      // opens the valve only if setAirflowForNote decides the note actually sounds,
      // and honours the minimum note duration. Schedule an automatic note-off so
      // the preview stops on its own.
      _instrument->postCommand(ACMD_NOTE_ON, note, _webVelocity);
      _testNoteMidi = note;
      _testNoteOffTime = millis() + TEST_NOTE_DURATION_MS;
    }
  } else if (strcmp(type, "pump_enable") == 0) {
    _instrument->postCommand(ACMD_PUMP_ENABLE, (doc["v"] | 1) != 0 ? 1 : 0);
  } else if (strcmp(type, "pump_target") == 0) {
    int pumpIdx = doc["pump"] | -1;
    if (pumpIdx >= 0) {
      _instrument->postCommand(ACMD_PUMP_SINGLE_TEST, (uint8_t)pumpIdx, getPercent(doc, "v", 0));
    } else {
      _instrument->postCommand(ACMD_PUMP_TARGET, 0, getPercent(doc, "v", 0));
    }
  } else if (strcmp(type, "pump_stop") == 0) {
    int pumpIdx = doc["pump"] | -1;
    _instrument->postCommand(pumpIdx >= 0 ? ACMD_PUMP_STOP_SINGLE : ACMD_PUMP_STOP);
    endTestSession(false);
  } else if (strcmp(type, "fan_target") == 0) {
    _instrument->postCommand(ACMD_FAN_TARGET, 0, getPercent(doc, "v", 0));
  } else if (strcmp(type, "fan_stop") == 0) {
    _instrument->postCommand(ACMD_FAN_STOP);
    endTestSession(false);
#if MIC_ENABLED
  } else if (strcmp(type, "mic_mon") == 0) {
    WebOp op; op.type = WEBOP_MIC_MONITOR; op.intA = ((doc["on"] | 0) != 0) ? 1 : 0;
    postWebOp(op);
  } else if (strcmp(type, "noise_cal") == 0) {
    const char* mode = doc["mode"] | "";
    WebOp op;
    if (strcmp(mode, "start") == 0) op.type = WEBOP_NOISE_START;
    else if (strcmp(mode, "stop") == 0) op.type = WEBOP_NOISE_STOP;
    else if (strcmp(mode, "reset") == 0) op.type = WEBOP_NOISE_RESET;
    else { client->text("{\"t\":\"error\",\"msg\":\"bad_mode\"}"); return; }
    op.clientId = client->id();
    postWebOp(op);
  } else if (strcmp(type, "mic_reset") == 0) {
    // Le resultat est diffuse par loop() sur le WebSocket (pas d'attente ici).
    WebOp op; op.type = WEBOP_MIC_RESET;
    postWebOp(op);
  } else if (strcmp(type, "auto_cal") == 0) {
    const char* mode = doc["mode"] | "";
    if (strcmp(mode, "air") == 0 || strcmp(mode, "range") == 0) {
      WebOp op;
      op.type = (strcmp(mode, "air") == 0) ? WEBOP_AUTOCAL_START_AIR : WEBOP_AUTOCAL_START_RANGE;
      op.clientId = client->id();
      postWebOp(op);
    } else if (strcmp(mode, "stop") == 0) {
      if (_autoCal && _autoCal->isRunning() && client->id() != _autoCalOwnerClientId) {
        client->text("{\"t\":\"error\",\"msg\":\"not_calibration_owner\"}");
      } else {
        requestCalibrationCancel();
      }
    } else if (strcmp(mode, "apply_range") == 0) {
      if (_autoCal && _autoCal->isRangeFinderComplete()) {
        if (_autoCalOwnerClientId != 0 && client->id() != _autoCalOwnerClientId) {
          client->text("{\"t\":\"error\",\"msg\":\"not_calibration_owner\"}");
        } else {
          WebOp op; op.type = WEBOP_AUTOCAL_APPLY_RANGE;
          postWebOp(op);
        }
      }
    }
  } else if (strcmp(type, "auto_stop") == 0) {
    if (_autoCal && _autoCal->isRunning() && client->id() != _autoCalOwnerClientId) {
      client->text("{\"t\":\"error\",\"msg\":\"not_calibration_owner\"}");
    } else {
      requestCalibrationCancel();
    }
#endif
  } else {
    client->text("{\"t\":\"error\",\"msg\":\"Unknown message type\"}");
  }
}

void WebConfigurator::broadcastStatus() {
  if (_ws.count() == 0) return;

  // Serialise par ArduinoJson : le statut porte des valeurs runtime et, via les
  // controleurs, des chaines issues de la configuration. La concatenation
  // manuelle ne garantissait pas leur echappement.
  JsonDocument doc;
  doc["t"] = "status";

  if (_instrument) {
    NoteSequencer& seq = _instrument->getSequencer();
    doc["playing"] = seq.isPlaying();
    doc["state"] = (int)seq.getState();
    doc["cc7"] = _instrument->getCCVolume();
    doc["cc11"] = _instrument->getCCExpression();
    doc["cc1"] = _instrument->getCCModulation();
    doc["cc2"] = _instrument->getCCBreath();
    doc["hw_ready"] = _instrument->isHardwareReady();
  } else {
    doc["hw_ready"] = false;
  }

  if (_player) {
    doc["ps"] = (int)_player->getState();
    if (_player->isFileLoaded()) {
      doc["pp"] = _player->getProgressPercent();
      doc["ppos"] = _player->getPositionMs();
    }
  }

  // Air system live data
  if (_instrument && cfg.airMode >= AIR_MODE_PUMP_VALVE) {
    PressureController& pc = _instrument->getPressureCtrl();
    doc["pump_pwm"] = pc.getPumpPwm();
    doc["res_pct"] = pc.getFillPercent();
    doc["res_mm"] = pc.getDistanceMm();
    doc["hall_val"] = pc.getHallValue();
    doc["endstop_st"] = pc.isEndstopActive();
    doc["sens_ok"] = pc.isSensorDetected();
    doc["sens_state"] = pc.sensorStateName();
    doc["sens_stale"] = pc.isMeasurementStale();
    doc["active_pumps"] = pc.getActivePumpCount();
    doc["bb_on"] = pc.isBangbangOn();
  }
  // Fan live data
  if (_instrument && cfg.airMode == AIR_MODE_FAN_SERVO) {
    FanController& fc = _instrument->getFanCtrl();
    doc["fan_pwm"] = fc.getPwm();
    doc["fan_speed"] = fc.getSpeed();
    doc["fan_ready"] = fc.isReady();
    doc["fan_idle"] = fc.isIdle();
  }
  if (_instrument) {
    doc["valve_open"] = _instrument->getAirflowCtrl().isValveOpen();
    doc["air_angle"] = _instrument->getAirflowCtrl().getAirflowAngle();
    if (strcmp(cfg.embouchure, "trav") == 0) {
      doc["ang_angle"] = _instrument->getAirflowCtrl().getAngleServoAngle();
    }
  }

  doc["heap"] = ESP.getFreeHeap();

  String json;
  serializeJson(doc, json);
  _ws.textAll(json);
}

#include "WifiMidiHandler.h"
#include "InstrumentManager.h"
#include "ConfigStorage.h"
#include "DeviceSecrets.h"
#include "gmb/GmbRuntime.h"

#include <ArduinoJson.h>

#include <WiFi.h>
#include <ESPmDNS.h>

// AppleMIDI / rtpMIDI
#include <AppleMIDI.h>

USING_NAMESPACE_APPLEMIDI;

static const byte DNS_PORT = 53;

// Instance rtpMIDI globale
APPLEMIDI_CREATE_DEFAULTSESSION_INSTANCE();

// Instance statique pour les callbacks
WifiMidiHandler* WifiMidiHandler::_instance = nullptr;

WifiMidiHandler::WifiMidiHandler()
  : _instrument(nullptr), _state(WIFI_STATE_DISCONNECTED),
    _connectStartTime(0), _sessionActive(false),
    _mdnsStarted(false), _rtpMidiStarted(false), _captiveDnsStarted(false) {
  _instance = this;
}

void WifiMidiHandler::begin(InstrumentManager* instrument) {
  _instrument = instrument;

  if (DEBUG) {
    Serial.println("DEBUG: WifiMidiHandler - Initialisation WiFi MIDI");
  }

  // Par defaut, demarrer en mode AP (hotspot)
  // Le WirelessManager decidera ensuite du mode STA si des credentials existent
  startAP();
}

void WifiMidiHandler::update() {
  // Traiter les requetes DNS captive portal (AP uniquement)
  if (_state == WIFI_STATE_AP_ACTIVE) {
    _dnsServer.processNextRequest();
  }

  // Verifier timeout connexion STA
  if (_state == WIFI_STATE_CONNECTING) {
    if (WiFi.status() == WL_CONNECTED) {
      _state = WIFI_STATE_STA_CONNECTED;

      if (DEBUG) {
        Serial.print("DEBUG: WifiMidiHandler - Connecte! IP: ");
        Serial.println(WiFi.localIP());
      }

      // Le lien est etabli : (re)demarrer mDNS + rtpMIDI une seule fois.
      startNetworkServices();
    } else if ((millis() - _connectStartTime) >= WIFI_CONNECT_TIMEOUT_MS) {
      // Timeout : fallback vers AP
      if (DEBUG) {
        Serial.println("DEBUG: WifiMidiHandler - Timeout connexion, fallback AP");
      }
      startAP();
    }
  }

  // Surveiller la chute d'une connexion STA etablie : si le lien Wi-Fi tombe
  // (routeur/AP disparu), la session rtpMIDI meurt sans garantie de callback
  // AppleMIDI. startAP() coupe le son (handleTransportLost) avant de demonter
  // les services reseau, puis remonte le hotspot.
  if (_state == WIFI_STATE_STA_CONNECTED && WiFi.status() != WL_CONNECTED) {
    if (DEBUG) {
      Serial.println("DEBUG: WifiMidiHandler - Lien STA perdu -> panic + fallback AP");
    }
    startAP();
    return;
  }

  // Lire les messages rtpMIDI entrants
  if (_state == WIFI_STATE_STA_CONNECTED || _state == WIFI_STATE_AP_ACTIVE) {
    MIDI.read();
  }
}

void WifiMidiHandler::stopNetworkServices(bool notifyTransportLost) {
  // 1. Couper le son AVANT tout demontage. Une note tenue via rtpMIDI ne
  //    recevra jamais son Note Off une fois la session fermee : sans ce panic,
  //    la valve, le souffle, la pompe et le ventilateur resteraient actifs.
  if (notifyTransportLost && _sessionActive && _instrument != nullptr) {
    _instrument->handleTransportLost();
  }
  _sessionActive = false;

  // 2. Demonter les services, chacun une seule fois. L'ancien code appelait
  //    MDNS.begin() a chaque bascule sans jamais MDNS.end() : les annonces
  //    s'empilaient et le nom d'hote pouvait rester sur l'ancienne interface.
  if (_captiveDnsStarted) {
    stopCaptiveDNS();
    _captiveDnsStarted = false;
  }
  if (_mdnsStarted) {
    MDNS.end();
    _mdnsStarted = false;
  }
  // AppleMIDI est un objet global : on ne le detruit pas, mais ses sockets UDP
  // sont lies a l'interface reseau qu'on vient d'arreter. Le drapeau est donc
  // remis a zero pour que startNetworkServices() les relie EXACTEMENT UNE FOIS
  // apres la bascule. C'est la re-initialisation multiple et incoherente qui
  // posait probleme, pas la re-initialisation elle-meme : l'ancien code appelait
  // setupRtpMidi() et MDNS.begin() a chaque passage sans jamais rien arreter.
  // (WiFiUDP::begin() ferme le socket precedent avant d'en ouvrir un nouveau,
  // donc ce second appel ne fuit pas de descripteur.)
  _rtpMidiStarted = false;
}

void WifiMidiHandler::startNetworkServices() {
  if (!_mdnsStarted) {
    setupMDNS();
    _mdnsStarted = true;
  }
  if (!_rtpMidiStarted) {
    setupRtpMidi();
    _rtpMidiStarted = true;
  }
  if (_state == WIFI_STATE_AP_ACTIVE && !_captiveDnsStarted) {
    startCaptiveDNS();
    _captiveDnsStarted = true;
  }
}

void WifiMidiHandler::startSTA(const char* ssid, const char* password) {
  if (DEBUG) {
    Serial.print("DEBUG: WifiMidiHandler - Connexion a: ");
    Serial.println(ssid);
  }

  // Demontage centralise (panic si une session pouvait etre active, arret du DNS
  // captif et de mDNS) avant de changer de mode radio.
  stopNetworkServices(true);

  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  _state = WIFI_STATE_CONNECTING;
  _connectStartTime = millis();
}

void WifiMidiHandler::startAP() {
  if (DEBUG) {
    Serial.print("DEBUG: WifiMidiHandler - Demarrage AP: ");
    Serial.println(AP_SSID);
  }

  // Demontage centralise (panic + arret des services) avant de basculer la radio.
  stopNetworkServices(true);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);

  // Ne JAMAIS ouvrir un hotspot non chiffre. La cle vient d'un VRAI secret
  // aleatoire genere au premier demarrage et conserve en NVS (DeviceSecrets) :
  // elle n'est derivee ni du MAC ni du BSSID, qui sont diffuses en clair dans
  // chaque trame 802.11 et ne sont donc pas des secrets.
  String apPass = AP_PASSWORD;
  bool generated = false;
  if (apPass.length() < 8) {
    apPass = DeviceSecrets::apPassword();
    generated = true;
  }
  WiFi.softAP(AP_SSID, apPass.c_str(), AP_CHANNEL, false, AP_MAX_CONNECTIONS);

  _state = WIFI_STATE_AP_ACTIVE;

  // Toujours afficher la cle (meme hors DEBUG) pour un appareil headless.
  Serial.print("WifiMidiHandler - AP WPA2 '");
  Serial.print(AP_SSID);
  Serial.print(generated ? "' (cle generee, stockee en NVS): " : "' (cle compilee): ");
  Serial.println(apPass);

  if (DEBUG) {
    Serial.print("DEBUG: WifiMidiHandler - AP actif, IP: ");
    Serial.println(WiFi.softAPIP());
  }

  // mDNS + rtpMIDI + DNS captif, chacun demarre une seule fois.
  startNetworkServices();
}

void WifiMidiHandler::forceAP() {
  // forceAP() peut arriver pendant une note (appui long sur BOOT). startAP()
  // enchaine panic -> demontage -> remontage, donc rien ne reste bloque.
  startAP();
}

WifiState WifiMidiHandler::getState() const {
  return _state;
}

bool WifiMidiHandler::isConnected() const {
  return _state == WIFI_STATE_STA_CONNECTED || _state == WIFI_STATE_AP_ACTIVE;
}

bool WifiMidiHandler::isAPMode() const {
  return _state == WIFI_STATE_AP_ACTIVE;
}

String WifiMidiHandler::getIPAddress() const {
  if (_state == WIFI_STATE_STA_CONNECTED) {
    return WiFi.localIP().toString();
  } else if (_state == WIFI_STATE_AP_ACTIVE) {
    return WiFi.softAPIP().toString();
  }
  return "0.0.0.0";
}

// Reduit un nom d'appareil libre a une etiquette DNS legale (a-z, 0-9, tirets).
static String sanitizeHostname(const char* name) {
  String out;
  for (const char* p = name; *p; p++) {
    char c = *p;
    if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      out += c;
    } else if ((c == '-' || c == '_' || c == ' ') && out.length() > 0 &&
               out[out.length() - 1] != '-') {
      out += '-';
    }
  }
  while (out.length() > 0 && out[out.length() - 1] == '-') {
    out.remove(out.length() - 1);
  }
  return out;
}

void WifiMidiHandler::setupMDNS() {
  // Utiliser le nom configure (assaini en etiquette DNS), sinon le nom compile.
  String host = sanitizeHostname(cfg.deviceName);
  if (host.length() == 0) host = MDNS_HOSTNAME;

  if (MDNS.begin(host.c_str())) {
    // Annoncer les services
    MDNS.addService("apple-midi", "udp", RTPMIDI_PORT);
    MDNS.addService("http", "tcp", WEB_SERVER_PORT);

    if (DEBUG) {
      Serial.print("DEBUG: WifiMidiHandler - mDNS actif: ");
      Serial.print(host);
      Serial.println(".local");
    }
  } else {
    if (DEBUG) {
      Serial.println("ERREUR: WifiMidiHandler - Echec mDNS");
    }
  }
}

void WifiMidiHandler::setupRtpMidi() {
  // Configurer les callbacks MIDI
  MIDI.setHandleNoteOn(onNoteOn);
  MIDI.setHandleNoteOff(onNoteOff);
  MIDI.setHandleControlChange(onControlChange);
  // General-Midi-Boop discovery. The callback only stages the request; the reply
  // is built and sent from the main loop (AppleMIDI flushes its out buffer at the
  // start of the next MIDI.read(), so nothing is written mid-parse).
  MIDI.setHandleSystemExclusive(onSystemExclusive);

  // Callbacks de session AppleMIDI
  AppleMIDI.setHandleConnected([](const APPLEMIDI_NAMESPACE::ssrc_t& ssrc, const char* name) {
    onAppleMidiConnected(name);
  });
  AppleMIDI.setHandleDisconnected([](const APPLEMIDI_NAMESPACE::ssrc_t& ssrc) {
    onAppleMidiDisconnected();
  });

  // Demarrer MIDI
  MIDI.begin(MIDI_CHANNEL_OMNI);

  if (DEBUG) {
    Serial.print("DEBUG: WifiMidiHandler - rtpMIDI pret sur port ");
    Serial.println(RTPMIDI_PORT);
  }
}

// --- Callbacks statiques ---

void WifiMidiHandler::onNoteOn(byte channel, byte note, byte velocity) {
  if (_instance == nullptr || _instance->_instrument == nullptr) return;
  if (!_instance->isChannelAccepted(channel)) return;

  if (velocity > 0) {
    _instance->_instrument->noteOn(note, velocity);
  } else {
    _instance->_instrument->noteOff(note);
  }
}

void WifiMidiHandler::onNoteOff(byte channel, byte note, byte velocity) {
  if (_instance == nullptr || _instance->_instrument == nullptr) return;
  if (!_instance->isChannelAccepted(channel)) return;

  _instance->_instrument->noteOff(note);
}

void WifiMidiHandler::onControlChange(byte channel, byte number, byte value) {
  if (_instance == nullptr || _instance->_instrument == nullptr) return;
  if (!_instance->isChannelAccepted(channel)) return;

  _instance->_instrument->handleControlChange(number, value);
}

void WifiMidiHandler::onSystemExclusive(byte* data, unsigned size) {
  if (_instance == nullptr) return;
  gmb::runtime::bridge().onSysEx(_instance, (const uint8_t*)data, (size_t)size);
}

void WifiMidiHandler::sendSysEx(const uint8_t* data, size_t len) {
  if (!_sessionActive || data == nullptr || len < 2) return;
  // The buffer already carries F0 ... F7. AppleMIDI splits a long message into
  // RFC 4695 continuation blocks by itself, so a 210-byte descriptor segment is
  // delivered whole.
  MIDI.sendSysEx((unsigned)len, data, true);
}

void WifiMidiHandler::onAppleMidiConnected(const char* name) {
  if (_instance != nullptr) _instance->_sessionActive = true;
  if (DEBUG) {
    Serial.print("DEBUG: WifiMidiHandler - rtpMIDI connecte: ");
    Serial.println(name);
  }
}

void WifiMidiHandler::onAppleMidiDisconnected() {
  // Panic : la session rtpMIDI est tombee ; une note tenue ne recevra pas son
  // Note Off -> couper le son (valve/souffle/pompe/ventilateur).
  if (_instance != nullptr) {
    _instance->_sessionActive = false;
    if (_instance->_instrument != nullptr) {
      _instance->_instrument->handleTransportLost();
    }
  }

  if (DEBUG) {
    Serial.println("DEBUG: WifiMidiHandler - rtpMIDI deconnecte");
  }
}

void WifiMidiHandler::startWifiScan() {
  WiFi.scanDelete();
  WiFi.scanNetworks(true);  // async

  if (DEBUG) {
    Serial.println("DEBUG: WifiMidiHandler - Scan WiFi lance (async)");
  }
}

bool WifiMidiHandler::isScanComplete() const {
  int result = WiFi.scanComplete();
  return (result != WIFI_SCAN_RUNNING);
}

String WifiMidiHandler::getScanResultsJson() const {
  // Les SSID sont controles par les AP environnants : ils peuvent contenir des
  // guillemets, des antislashs ou des octets de controle. La serialisation passe
  // donc par ArduinoJson, qui echappe correctement, plutot que par une
  // concatenation manuelle.
  int n = WiFi.scanComplete();
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  if (n > 0) {
    for (int i = 0; i < n; i++) {
      JsonObject net = arr.add<JsonObject>();
      net["ssid"] = WiFi.SSID(i);
      net["rssi"] = WiFi.RSSI(i);
      net["enc"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN) ? 1 : 0;
    }
  }
  String json;
  serializeJson(doc, json);
  WiFi.scanDelete();
  return json;
}

void WifiMidiHandler::connectToNetwork(const char* ssid, const char* password) {
  if (DEBUG) {
    Serial.print("DEBUG: WifiMidiHandler - Connexion vers: ");
    Serial.println(ssid);
  }
  // NB: la persistance des identifiants n'est PLUS faite ici. Cette methode est
  // appelee depuis la tache loop() par le commit transactionnel de
  // /api/wifi/connect, qui ecrit la configuration avant de demander la bascule.
  // startSTA() effectue lui-meme le demontage centralise (panic + services).
  startSTA(ssid, password);
}

void WifiMidiHandler::startCaptiveDNS() {
  _dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());

  if (DEBUG) {
    Serial.println("DEBUG: WifiMidiHandler - DNS captive portal actif");
  }
}

void WifiMidiHandler::stopCaptiveDNS() {
  _dnsServer.stop();

  if (DEBUG) {
    Serial.println("DEBUG: WifiMidiHandler - DNS captive portal arrete");
  }
}

bool WifiMidiHandler::isChannelAccepted(byte channel) {
  if (cfg.midiChannel == 0) return true;
  return (channel == cfg.midiChannel);
}

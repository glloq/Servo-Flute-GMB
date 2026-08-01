#include "WifiMidiHandler.h"
#include "InstrumentManager.h"
#include "ConfigStorage.h"

#include <WiFi.h>
#include <ESPmDNS.h>

// AppleMIDI / rtpMIDI
#include <AppleMIDI.h>

USING_NAMESPACE_APPLEMIDI;

static const byte DNS_PORT = 53;

// Echappe une chaine pour l'inserer entre guillemets dans du JSON. Les SSID sont
// controles par les AP environnants et peuvent contenir " ou \ (ou des octets
// de controle) qui, sans echappement, cassent la reponse du scan Wi-Fi.
static String jsonEscape(const String& s) {
  String out;
  out.reserve(s.length() + 2);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if ((uint8_t)c < 0x20) {
          char buf[7];
          snprintf(buf, sizeof(buf), "\\u%04x", (uint8_t)c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

// Instance rtpMIDI globale
APPLEMIDI_CREATE_DEFAULTSESSION_INSTANCE();

// Instance statique pour les callbacks
WifiMidiHandler* WifiMidiHandler::_instance = nullptr;

WifiMidiHandler::WifiMidiHandler()
  : _instrument(nullptr), _state(WIFI_STATE_DISCONNECTED),
    _connectStartTime(0) {
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

      // Configurer mDNS et rtpMIDI apres connexion
      setupMDNS();
      setupRtpMidi();
    } else if ((millis() - _connectStartTime) >= WIFI_CONNECT_TIMEOUT_MS) {
      // Timeout : fallback vers AP
      if (DEBUG) {
        Serial.println("DEBUG: WifiMidiHandler - Timeout connexion, fallback AP");
      }
      WiFi.disconnect();
      startAP();
    }
  }

  // Surveiller la chute d'une connexion STA etablie : si le lien Wi-Fi tombe
  // (routeur/AP disparu), la session rtpMIDI meurt sans garantie de callback
  // AppleMIDI. On coupe le son puis on retombe en mode AP.
  if (_state == WIFI_STATE_STA_CONNECTED && WiFi.status() != WL_CONNECTED) {
    if (DEBUG) {
      Serial.println("DEBUG: WifiMidiHandler - Lien STA perdu -> panic + fallback AP");
    }
    if (_instrument != nullptr) {
      _instrument->handleTransportLost();
    }
    startAP();
    return;
  }

  // Lire les messages rtpMIDI entrants
  if (_state == WIFI_STATE_STA_CONNECTED || _state == WIFI_STATE_AP_ACTIVE) {
    MIDI.read();
  }
}

void WifiMidiHandler::startSTA(const char* ssid, const char* password) {
  if (DEBUG) {
    Serial.print("DEBUG: WifiMidiHandler - Connexion a: ");
    Serial.println(ssid);
  }

  // Arreter le DNS captive portal si on quitte le mode AP
  if (_state == WIFI_STATE_AP_ACTIVE) {
    stopCaptiveDNS();
  }

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

  WiFi.mode(WIFI_AP);

  // Ne JAMAIS ouvrir un hotspot non chiffre : sans mot de passe configure (ou
  // trop court pour du WPA2), on en derive un stable a partir du MAC du chip.
  // Un client doit connaitre cette cle pour atteindre l'API/WebSocket.
  String apPass = AP_PASSWORD;
  bool generated = false;
  if (apPass.length() < 8) {
    uint32_t id = (uint32_t)(ESP.getEfuseMac() & 0xFFFFFF);
    char buf[16];
    snprintf(buf, sizeof(buf), "flute-%06X", id);
    apPass = buf;
    generated = true;
  }
  WiFi.softAP(AP_SSID, apPass.c_str(), AP_CHANNEL, false, AP_MAX_CONNECTIONS);

  _state = WIFI_STATE_AP_ACTIVE;

  // Toujours afficher la cle (meme hors DEBUG) pour un appareil headless.
  Serial.print("WifiMidiHandler - AP WPA2 '");
  Serial.print(AP_SSID);
  Serial.print(generated ? "' (mot de passe genere): " : "' (mot de passe configure): ");
  Serial.println(apPass);

  if (DEBUG) {
    Serial.print("DEBUG: WifiMidiHandler - AP actif, IP: ");
    Serial.println(WiFi.softAPIP());
  }

  // Configurer mDNS et rtpMIDI en mode AP aussi
  setupMDNS();
  setupRtpMidi();

  // Demarrer le DNS captive portal
  startCaptiveDNS();
}

void WifiMidiHandler::forceAP() {
  if (_state == WIFI_STATE_STA_CONNECTED || _state == WIFI_STATE_CONNECTING) {
    WiFi.disconnect();
  }
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

void WifiMidiHandler::onAppleMidiConnected(const char* name) {
  if (DEBUG) {
    Serial.print("DEBUG: WifiMidiHandler - rtpMIDI connecte: ");
    Serial.println(name);
  }
}

void WifiMidiHandler::onAppleMidiDisconnected() {
  // Panic : la session rtpMIDI est tombee ; une note tenue ne recevra pas son
  // Note Off -> couper le son (valve/souffle/pompe/ventilateur).
  if (_instance != nullptr && _instance->_instrument != nullptr) {
    _instance->_instrument->handleTransportLost();
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
  int n = WiFi.scanComplete();
  String json = "[";

  if (n > 0) {
    for (int i = 0; i < n; i++) {
      if (i > 0) json += ",";
      json += "{\"ssid\":\"" + jsonEscape(WiFi.SSID(i)) + "\"";
      json += ",\"rssi\":" + String(WiFi.RSSI(i));
      json += ",\"enc\":" + String(WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? 1 : 0);
      json += "}";
    }
  }

  json += "]";
  WiFi.scanDelete();
  return json;
}

void WifiMidiHandler::connectToNetwork(const char* ssid, const char* password) {
  if (DEBUG) {
    Serial.print("DEBUG: WifiMidiHandler - Connexion vers: ");
    Serial.println(ssid);
  }

  // Sauvegarder dans config
  strncpy(cfg.wifiSsid, ssid, sizeof(cfg.wifiSsid) - 1);
  cfg.wifiSsid[sizeof(cfg.wifiSsid) - 1] = '\0';
  strncpy(cfg.wifiPassword, password, sizeof(cfg.wifiPassword) - 1);
  cfg.wifiPassword[sizeof(cfg.wifiPassword) - 1] = '\0';
  ConfigStorage::save();

  // Deconnecter et reconnecter en STA
  if (_state == WIFI_STATE_AP_ACTIVE || _state == WIFI_STATE_CONNECTING) {
    WiFi.disconnect();
    delay(100);
  }

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

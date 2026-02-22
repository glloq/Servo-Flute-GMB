#include "WifiMidiHandler.h"
#include "InstrumentManager.h"
#include "ConfigStorage.h"

#include <WiFi.h>
#include <ESPmDNS.h>

// AppleMIDI / rtpMIDI
#include <AppleMIDI.h>

USING_NAMESPACE_APPLEMIDI;

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

  if (strlen(AP_PASSWORD) > 0) {
    WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, false, AP_MAX_CONNECTIONS);
  } else {
    WiFi.softAP(AP_SSID, NULL, AP_CHANNEL, false, AP_MAX_CONNECTIONS);
  }

  _state = WIFI_STATE_AP_ACTIVE;

  if (DEBUG) {
    Serial.print("DEBUG: WifiMidiHandler - AP actif, IP: ");
    Serial.println(WiFi.softAPIP());
  }

  // Configurer mDNS et rtpMIDI en mode AP aussi
  setupMDNS();
  setupRtpMidi();
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

void WifiMidiHandler::setupMDNS() {
  if (MDNS.begin(MDNS_HOSTNAME)) {
    // Annoncer les services
    MDNS.addService("apple-midi", "udp", RTPMIDI_PORT);
    MDNS.addService("http", "tcp", WEB_SERVER_PORT);

    if (DEBUG) {
      Serial.print("DEBUG: WifiMidiHandler - mDNS actif: ");
      Serial.print(MDNS_HOSTNAME);
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
      json += "{\"ssid\":\"" + WiFi.SSID(i) + "\"";
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

bool WifiMidiHandler::isChannelAccepted(byte channel) {
  if (cfg.midiChannel == 0) return true;
  return (channel == cfg.midiChannel);
}

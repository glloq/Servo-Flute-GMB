#include "WirelessManager.h"
#include "InstrumentManager.h"
#include "ConfigStorage.h"

WirelessManager::WirelessManager(StatusLed& led, HardwareInputs& inputs)
  : _led(led), _inputs(inputs), _instrument(nullptr),
    _currentMode(MODE_BLUETOOTH),
    _webConfig(nullptr), _midiPlayer(nullptr) {
}

void WirelessManager::begin(InstrumentManager* instrument) {
  _instrument = instrument;
  _currentMode = _inputs.getMode();

  if (DEBUG) {
    Serial.println("========================================");
    Serial.print("   MODE: ");
    Serial.println(_currentMode == MODE_BLUETOOTH ? "BLUETOOTH (BLE-MIDI)" : "WIFI (rtpMIDI)");
    Serial.println("========================================");
  }

  if (_currentMode == MODE_BLUETOOTH) {
    // Mode BLE-MIDI
    _bleMidi.begin(instrument);
    _led.setPattern(LED_BLINK_FAST);  // Advertising actif

  } else {
    // Mode WiFi - tenter STA si credentials sauvegardees, sinon AP
    _wifiMidi.begin(instrument);

    if (strlen(cfg.wifiSsid) > 0) {
      if (DEBUG) {
        Serial.print("DEBUG: WirelessManager - Tentative STA: ");
        Serial.println(cfg.wifiSsid);
      }
      _wifiMidi.startSTA(cfg.wifiSsid, cfg.wifiPassword);
      _led.setPattern(LED_BLINK_FAST);  // Connexion en cours
    } else {
      _led.setPattern(LED_TRIPLE_FLASH);  // Mode AP
    }

    // Initialiser le lecteur MIDI
    _midiPlayer = new MidiFilePlayer();
    _midiPlayer->begin(instrument);

    // Initialiser le serveur web (apres WiFi pour que le reseau soit pret)
    _webConfig = new WebConfigurator();
    _webConfig->setWirelessManager(this);
    _webConfig->begin(instrument, _midiPlayer);

    if (DEBUG) {
      Serial.println("DEBUG: WirelessManager - Serveur web + lecteur MIDI initialises");
    }
  }
}

void WirelessManager::update() {
  // Lire les evenements bouton
  ButtonEvent event = _inputs.getButtonEvent();
  if (event != BUTTON_NONE) {
    handleButtonEvent(event);
  }

  // Mettre a jour le handler MIDI actif
  if (_currentMode == MODE_BLUETOOTH) {
    _bleMidi.update();
  } else {
    _wifiMidi.update();

    // Mettre a jour le lecteur MIDI (playback non-bloquant)
    if (_midiPlayer) {
      _midiPlayer->update();
    }

    // Mettre a jour le serveur web (status broadcast, cleanup WS)
    if (_webConfig) {
      _webConfig->update();
    }
  }

  // Mettre a jour le pattern LED
  updateLedPattern();
}

OperatingMode WirelessManager::getMode() const {
  return _currentMode;
}

bool WirelessManager::isMidiConnected() const {
  if (_currentMode == MODE_BLUETOOTH) {
    return _bleMidi.isConnected();
  } else {
    return _wifiMidi.isConnected();
  }
}

String WirelessManager::getStatusText() const {
  if (_currentMode == MODE_BLUETOOTH) {
    if (_bleMidi.isConnected()) {
      return "BLE: Connecte";
    } else if (_bleMidi.isAdvertising()) {
      return "BLE: Advertising...";
    }
    return "BLE: Inactif";
  } else {
    if (_wifiMidi.isAPMode()) {
      return "WiFi AP: " + _wifiMidi.getIPAddress();
    } else if (_wifiMidi.getState() == WIFI_STATE_STA_CONNECTED) {
      return "WiFi: " + _wifiMidi.getIPAddress();
    } else if (_wifiMidi.getState() == WIFI_STATE_CONNECTING) {
      return "WiFi: Connexion...";
    }
    return "WiFi: Deconnecte";
  }
}

void WirelessManager::handleButtonEvent(ButtonEvent event) {
  if (_currentMode == MODE_BLUETOOTH) {
    if (event == BUTTON_SHORT_PRESS) {
      if (DEBUG) {
        Serial.println("DEBUG: WirelessManager - Restart advertising BLE");
      }
      _bleMidi.startAdvertising();
    }
    if (event == BUTTON_LONG_PRESS) {
      if (DEBUG) {
        Serial.println("DEBUG: WirelessManager - Appui long en mode BT (ignore)");
      }
    }

  } else {
    // Mode WiFi
    if (event == BUTTON_SHORT_PRESS) {
      if (DEBUG) {
        Serial.print("DEBUG: WirelessManager - IP: ");
        Serial.println(_wifiMidi.getIPAddress());
      }
    }

    if (event == BUTTON_LONG_PRESS) {
      if (DEBUG) {
        Serial.println("DEBUG: WirelessManager - Forcer mode AP (hotspot)");
      }
      _wifiMidi.forceAP();
    }
  }
}

void WirelessManager::updateLedPattern() {
  if (_currentMode == MODE_BLUETOOTH) {
    if (_bleMidi.isConnected()) {
      _led.setPattern(LED_BLINK_SLOW);     // Connecte : cligno lent
    } else {
      _led.setPattern(LED_BLINK_FAST);     // Advertising : cligno rapide
    }
  } else {
    switch (_wifiMidi.getState()) {
      case WIFI_STATE_CONNECTING:
        _led.setPattern(LED_BLINK_FAST);    // Connexion en cours
        break;
      case WIFI_STATE_STA_CONNECTED:
        _led.setPattern(LED_DOUBLE_FLASH);  // WiFi STA connecte
        break;
      case WIFI_STATE_AP_ACTIVE:
        _led.setPattern(LED_TRIPLE_FLASH);  // Mode hotspot
        break;
      default:
        _led.setPattern(LED_BLINK_FAST);    // Deconnecte
        break;
    }
  }
}

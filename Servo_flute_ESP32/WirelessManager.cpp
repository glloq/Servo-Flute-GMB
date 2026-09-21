#include "WirelessManager.h"
#include "InstrumentManager.h"
#include "ConfigStorage.h"
#include "gmb/GmbRuntime.h"

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
    // BLE-MIDI carries SysEx both ways: register it as a General-Midi-Boop port.
    gmb::runtime::bridge().registerPort(&_bleMidi);
    _led.setPattern(LED_BLINK_FAST);  // Advertising actif

  } else {
    // Mode WiFi - tenter STA si credentials sauvegardees, sinon AP
    _wifiMidi.begin(instrument);
    // rtpMIDI carries SysEx both ways once a session is open.
    gmb::runtime::bridge().registerPort(&_wifiMidi);

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

  // Initialiser le MIDI serie (independant du mode BLE/WiFi).
  // Le MIDI DIN de cette carte est en RECEPTION SEULE (UART2 RX, pas de TX) : il
  // ne peut donc pas repondre a une requete SysEx et n'est pas enregistre comme
  // port GMB. Il continue de fonctionner normalement pour les Note / CC.
  _serialMidi.begin(instrument);
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

  // Mettre a jour le MIDI serie (independant du mode BLE/WiFi)
  _serialMidi.update();

  // Repondre a une eventuelle requete GMB mise en attente par un callback SysEx.
  // C'est du trafic de plan de controle : il est traite ici, hors du chemin
  // temps reel des notes, et jamais depuis le callback MIDI lui-meme.
  gmb::runtime::bridge().service(millis());

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
      return "BLE: Connected";
    } else if (_bleMidi.isAdvertising()) {
      return "BLE: Advertising...";
    }
    return "BLE: Inactive";
  } else {
    if (_wifiMidi.isAPMode()) {
      return "WiFi AP: " + _wifiMidi.getIPAddress();
    } else if (_wifiMidi.getState() == WIFI_STATE_STA_CONNECTED) {
      return "WiFi: " + _wifiMidi.getIPAddress();
    } else if (_wifiMidi.getState() == WIFI_STATE_CONNECTING) {
      return "WiFi: Connecting...";
    }
    return "WiFi: Disconnected";
  }
}

void WirelessManager::handleButtonEvent(ButtonEvent event) {
  // Double appui : ouvre tous les doigts (quel que soit le mode).
  // Ignore pendant une session d'actionneurs (calibration / test manuel) : celle-ci
  // possede les doigts et un appui direct corromprait la mesure en cours en luttant
  // contre le proprietaire de la session.
  if (event == BUTTON_DOUBLE_PRESS) {
    if (_instrument) {
      if (_instrument->isActuatorSessionActive()) {
        if (DEBUG) {
          Serial.println("DEBUG: WirelessManager - Double appui ignore (session actionneurs active)");
        }
        return;
      }
      // Passe par la file de commandes : c'est le seul chemin d'application des
      // ordres actionneurs, et il porte la protection hardware_not_ready.
      _instrument->postCommand(ACMD_OPEN_ALL_FINGERS);

      if (DEBUG) {
        Serial.println("DEBUG: WirelessManager - Double appui: ouverture tous les doigts");
      }
    }
    return;
  }

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

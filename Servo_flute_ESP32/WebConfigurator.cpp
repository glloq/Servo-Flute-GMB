#include "WebConfigurator.h"
#include "InstrumentManager.h"
#include "WirelessManager.h"
#include "web_content.h"
#include <WiFi.h>

WebConfigurator::WebConfigurator(uint16_t port)
  : _server(port), _ws("/ws"),
    _instrument(nullptr), _player(nullptr), _wirelessManager(nullptr),
    _webVelocity(WEB_DEFAULT_VELOCITY), _lastStatusBroadcast(0), _lastWsCleanup(0),
    _uploadSize(0)
#if MIC_ENABLED
    , _audio(nullptr), _autoCal(nullptr), _micMonitorEnabled(false), _lastAudioBroadcast(0), _lastAcalBroadcast(0)
#endif
{
}

WebConfigurator::~WebConfigurator() {
#if MIC_ENABLED
  delete _autoCal;
  delete _audio;
#endif
}

void WebConfigurator::begin(InstrumentManager* instrument, MidiFilePlayer* player) {
  _instrument = instrument;
  _player = player;

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

#if MIC_ENABLED
  // Initialize microphone (INMP441 via I2S)
  _audio = new AudioAnalyzer();
  bool micOk = _audio->begin();
  if (micOk && _instrument) {
    _autoCal = new AutoCalibrator(
      _instrument->getFingerCtrl(),
      _instrument->getAirflowCtrl(),
      *_audio);
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
    _audio->update();

    // Broadcast audio data if monitoring enabled
    if (_micMonitorEnabled && _audio->isActive() && _ws.count() > 0) {
      if (now - _lastAudioBroadcast >= AUTOCAL_AUDIO_INTERVAL_MS) {
        String aj = "{\"t\":\"audio\"";
        aj += ",\"rms\":" + String(_audio->getRMS(), 3);
        aj += ",\"snd\":" + String(_audio->isSoundDetected() ? 1 : 0);
        if (_audio->getPitchHz() > 0) {
          aj += ",\"hz\":" + String(_audio->getPitchHz(), 1);
          aj += ",\"midi\":" + String(_audio->getPitchMidi());
          aj += ",\"cents\":" + String(_audio->getPitchCents(), 1);
        }
        aj += "}";
        _ws.textAll(aj);
        _lastAudioBroadcast = now;
      }
    }

    // Update auto-calibrator
    if (_autoCal && _autoCal->isRunning()) {
      _autoCal->update();

      // Broadcast progress
      if (_ws.count() > 0 && now - _lastAcalBroadcast >= AUTOCAL_AUDIO_INTERVAL_MS) {
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
        pj += ",\"total\":" + String(cfg.numNotes);
        pj += ",\"angle\":" + String(_autoCal->getCurrentAngle());
        pj += ",\"st\":" + String((int)_autoCal->getState());
        pj += "}";
        _ws.textAll(pj);
        _lastAcalBroadcast = now;
      }
    }
    // Check completion
    if (_autoCal && _autoCal->isComplete()) {
      _autoCal->applyResults();
      const char* names[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
      String dj = "{\"t\":\"acal_done\",\"ok\":true,\"results\":[";
      for (int i = 0; i < cfg.numNotes; i++) {
        if (i > 0) dj += ",";
        byte midi = cfg.notes[i].midiNote;
        String nn = String(names[midi % 12]) + String((int)(midi / 12) - 1);
        AutoCalNoteResult r = _autoCal->getResult(i);
        dj += "{\"name\":\"" + nn + "\",\"ok\":" + String(r.valid ? "true" : "false");
        dj += ",\"min\":" + String(r.airMin) + ",\"max\":" + String(r.airMax) + "}";
      }
      dj += "]}";
      _ws.textAll(dj);
      _autoCal->stop();
    }
  }
#endif
}

void WebConfigurator::setWirelessManager(WirelessManager* wm) {
  _wirelessManager = wm;
}

void WebConfigurator::setupRoutes() {
  // Page principale
  _server.on("/", HTTP_GET, [this](AsyncWebServerRequest* request) {
    handleRoot(request);
  });

  // API Status
  _server.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest* request) {
    handleApiStatus(request);
  });

  // API Config GET
  _server.on("/api/config", HTTP_GET, [this](AsyncWebServerRequest* request) {
    handleApiConfig(request);
  });

  // API Config POST (body handler accumule, request handler traite)
  _server.on("/api/config", HTTP_POST,
    [this](AsyncWebServerRequest* request) {
      handleApiConfigFinalize(request);
    },
    NULL,
    [this](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
      // Accumuler le body (data n'est pas null-termine)
      if (index == 0) {
        _configBody = "";
        _configBody.reserve(total + 1);
      }
      char* buf = (char*)malloc(len + 1);
      if (buf) {
        memcpy(buf, data, len);
        buf[len] = '\0';
        _configBody += buf;
        free(buf);
      }
    }
  );

  // API Config Reset
  _server.on("/api/config/reset", HTTP_POST, [this](AsyncWebServerRequest* request) {
    handleApiConfigReset(request);
  });

  // API Factory Reset (supprime le fichier config pour relancer le wizard)
  _server.on("/api/config/factory", HTTP_POST, [this](AsyncWebServerRequest* request) {
    handleApiFactoryReset(request);
  });

  // API WiFi Scan (lance le scan)
  _server.on("/api/wifi/scan", HTTP_GET, [this](AsyncWebServerRequest* request) {
    if (_wirelessManager) {
      _wirelessManager->getWifiMidi().startWifiScan();
      request->send(200, "application/json", "{\"ok\":true,\"msg\":\"Scan lance\"}");
    } else {
      request->send(500, "application/json", "{\"ok\":false}");
    }
  });

  // API WiFi Scan Results
  _server.on("/api/wifi/results", HTTP_GET, [this](AsyncWebServerRequest* request) {
    if (_wirelessManager) {
      bool done = _wirelessManager->getWifiMidi().isScanComplete();
      String json = "{\"done\":" + String(done ? "true" : "false");
      if (done) {
        json += ",\"networks\":" + _wirelessManager->getWifiMidi().getScanResultsJson();
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
      if (_configBody.length() == 0) {
        request->send(400, "application/json", "{\"ok\":false,\"msg\":\"Body vide\"}");
      }
    },
    NULL,
    [this](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
      if (index == 0) { _configBody = ""; _configBody.reserve(total + 1); }
      char* buf = (char*)malloc(len + 1);
      if (buf) { memcpy(buf, data, len); buf[len] = '\0'; _configBody += buf; free(buf); }
      if (index + len == total) {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, _configBody);
        if (err || !doc.containsKey("ssid")) {
          request->send(400, "application/json", "{\"ok\":false,\"msg\":\"JSON invalide\"}");
        } else if (_wirelessManager) {
          const char* ssid = doc["ssid"];
          const char* pass = doc["pass"] | "";
          request->send(200, "application/json", "{\"ok\":true,\"msg\":\"Connexion en cours...\"}");
          _wirelessManager->getWifiMidi().connectToNetwork(ssid, pass);
        } else {
          request->send(500, "application/json", "{\"ok\":false}");
        }
        _configBody = "";
      }
    }
  );

  // API WiFi Status
  _server.on("/api/wifi/status", HTTP_GET, [this](AsyncWebServerRequest* request) {
    String json = "{";
    if (_wirelessManager) {
      WifiMidiHandler& wm = _wirelessManager->getWifiMidi();
      json += "\"state\":" + String(wm.getState());
      json += ",\"ip\":\"" + wm.getIPAddress() + "\"";
      json += ",\"ap\":" + String(wm.isAPMode() ? "true" : "false");
      json += ",\"ssid\":\"" + String(cfg.wifiSsid) + "\"";
      if (wm.getState() == WIFI_STATE_STA_CONNECTED) {
        json += ",\"rssi\":" + String(WiFi.RSSI());
      }
    }
    json += "}";
    request->send(200, "application/json", json);
  });

  // Upload MIDI (multipart)
  _server.on("/api/midi", HTTP_POST,
    [this](AsyncWebServerRequest* request) {
      handleMidiUploadComplete(request);
    },
    [this](AsyncWebServerRequest* request, const String& filename,
           size_t index, uint8_t* data, size_t len, bool final) {
      handleMidiUpload(request, filename, index, data, len, final);
    }
  );

  // MIDI file list
  _server.on("/api/midi/list", HTTP_GET, [this](AsyncWebServerRequest* request) {
    handleMidiList(request);
  });

  // MIDI file delete
  _server.on("/api/midi/delete", HTTP_POST,
    [this](AsyncWebServerRequest* request) {
      handleMidiDelete(request);
    },
    NULL,
    [this](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
      if (index == 0) { _configBody = ""; _configBody.reserve(total + 1); }
      char* buf = (char*)malloc(len + 1);
      if (buf) { memcpy(buf, data, len); buf[len] = '\0'; _configBody += buf; free(buf); }
    }
  );

  // MIDI file load (select for playback)
  _server.on("/api/midi/load", HTTP_POST,
    [this](AsyncWebServerRequest* request) {
      handleMidiLoad(request);
    },
    NULL,
    [this](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
      if (index == 0) { _configBody = ""; _configBody.reserve(total + 1); }
      char* buf = (char*)malloc(len + 1);
      if (buf) { memcpy(buf, data, len); buf[len] = '\0'; _configBody += buf; free(buf); }
    }
  );

  // 404
  _server.onNotFound([](AsyncWebServerRequest* request) {
    request->send(404, "text/plain", "Not found");
  });
}

void WebConfigurator::handleRoot(AsyncWebServerRequest* request) {
  AsyncWebServerResponse* response = request->beginResponse_P(200, "text/html", (const uint8_t*)WEB_HTML_CONTENT, sizeof(WEB_HTML_CONTENT) - 1);
  request->send(response);
}

void WebConfigurator::handleApiStatus(AsyncWebServerRequest* request) {
  String json = "{";
  json += "\"mode\":\"" + String(_wirelessManager ? _wirelessManager->getStatusText() : "N/A") + "\"";
  json += ",\"connected\":" + String(_wirelessManager ? (_wirelessManager->isMidiConnected() ? "true" : "false") : "false");
  json += ",\"uptime\":" + String(millis() / 1000);

  // CC values
  if (_instrument) {
    json += ",\"cc7\":" + String(_instrument->getCCVolume());
    json += ",\"cc11\":" + String(_instrument->getCCExpression());
    json += ",\"cc1\":" + String(_instrument->getCCModulation());
    json += ",\"cc2\":" + String(_instrument->getCCBreath());
  }

  // Player state
  if (_player) {
    json += ",\"player\":{";
    json += "\"state\":" + String(_player->getState());
    json += ",\"loaded\":" + String(_player->isFileLoaded() ? "true" : "false");
    json += ",\"file\":\"" + _player->getFileName() + "\"";
    json += ",\"events\":" + String(_player->getEventCount());
    json += ",\"duration\":" + String(_player->getDurationMs());
    json += ",\"position\":" + String(_player->getPositionMs());
    json += ",\"progress\":" + String(_player->getProgressPercent(), 1);
    json += "}";
  }

  json += "}";
  request->send(200, "application/json", json);
}

void WebConfigurator::handleApiConfig(AsyncWebServerRequest* request) {
  String json = "{";

  // Instrument info
  json += "\"num_fingers\":" + String(cfg.numFingers);
  json += ",\"num_notes\":" + String(cfg.numNotes);
  json += ",\"air_pca\":" + String(cfg.airflowPcaChannel);
  json += ",\"angle_open\":" + String(cfg.fingerAngleOpen);
  json += ",\"half_hole_pct\":" + String(cfg.halfHolePercent);
  json += ",\"embouchure\":\"" + String(cfg.embouchure) + "\"";

  // Scalaires
  json += ",\"midi_ch\":" + String(cfg.midiChannel);
  json += ",\"servo_delay\":" + String(cfg.servoToSolenoidDelayMs);
  json += ",\"valve_interval\":" + String(cfg.minNoteIntervalForValveCloseMs);
  json += ",\"min_note_dur\":" + String(cfg.minNoteDurationMs);
  json += ",\"air_off\":" + String(cfg.servoAirflowOff);
  json += ",\"air_min\":" + String(cfg.servoAirflowMin);
  json += ",\"air_max\":" + String(cfg.servoAirflowMax);
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
  json += ",\"device\":\"" + String(cfg.deviceName) + "\"";
  json += ",\"wifi_ssid\":\"" + String(cfg.wifiSsid) + "\"";
  json += ",\"time_unpower\":" + String(cfg.timeUnpower);
  json += ",\"hide_calib\":" + String(cfg.hideCalibration ? "true" : "false");
  json += ",\"sol_pin\":" + String(cfg.solenoidPin);
  json += ",\"kbd_mode\":" + String(cfg.kbdMode);
  json += ",\"color\":\"" + String(cfg.instrumentColor) + "\"";
  json += ",\"air_atk_mode\":" + String(cfg.airAttackMode);
  json += ",\"air_atk_off\":" + String(cfg.airAttackOffset);
  json += ",\"air_atk_ms\":" + String(cfg.airAttackMs);
  json += ",\"air_vel_resp\":" + String(cfg.airVelocityResponse);

  // Air delivery system
  json += ",\"air_mode\":" + String(cfg.airMode);
  json += ",\"valve_servo\":" + String(cfg.valveUseServo ? "true" : "false");
  json += ",\"valve_ch\":" + String(cfg.valveServoPcaChannel);
  json += ",\"pump_on\":" + String(cfg.pumpEnabled ? "true" : "false");
  json += ",\"pump_pin\":" + String(cfg.pumpPin);
  json += ",\"pump_min\":" + String(cfg.pumpMinPwm);
  json += ",\"pump_max\":" + String(cfg.pumpMaxPwm);
  json += ",\"res_on\":" + String(cfg.reservoirEnabled ? "true" : "false");
  json += ",\"sens_type\":" + String(cfg.sensorType);
  json += ",\"sens_target\":" + String(cfg.sensorTargetMm);
  json += ",\"sens_min\":" + String(cfg.sensorMinMm);
  json += ",\"sens_max\":" + String(cfg.sensorMaxMm);
  json += ",\"pid_kp\":" + String(cfg.pidKp);
  json += ",\"pid_ki\":" + String(cfg.pidKi);
  json += ",\"show_air\":" + String(cfg.showAirSystem ? "true" : "false");
  json += ",\"midi_limit\":" + String(cfg.midiStorageLimitKb);

#if MIC_ENABLED
  json += ",\"mic\":" + String((_audio && _audio->isMicDetected()) ? "true" : "false");
#else
  json += ",\"mic\":false";
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
    json += ",\"th\":" + String(cfg.fingers[i].isThumbHole ? 1 : 0) + "}";
  }
  json += "]";

  // Notes jouables (complete: MIDI + doigtes + airflow)
  json += ",\"notes\":[";
  for (int i = 0; i < cfg.numNotes; i++) {
    if (i > 0) json += ",";
    json += "{\"midi\":" + String(cfg.notes[i].midiNote);
    json += ",\"amn\":" + String(cfg.notes[i].airflowMinPercent);
    json += ",\"amx\":" + String(cfg.notes[i].airflowMaxPercent);
    json += ",\"fp\":[";
    for (int f = 0; f < cfg.numFingers; f++) {
      if (f > 0) json += ",";
      json += String((int)cfg.notes[i].fingerPattern[f]);
    }
    json += "]}";
  }
  json += "]";

  json += "}";
  request->send(200, "application/json", json);
}

void WebConfigurator::handleApiConfigFinalize(AsyncWebServerRequest* request) {
  if (_configBody.length() == 0) {
    request->send(400, "application/json", "{\"ok\":false,\"msg\":\"Body vide\"}");
    return;
  }

  {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, _configBody);

    if (err) {
      request->send(400, "application/json", "{\"ok\":false,\"msg\":\"JSON invalide\"}");
      _configBody = "";
      return;
    }

    // --- Instrument modulaire ---
    if (doc.containsKey("num_fingers")) {
      uint8_t nf = doc["num_fingers"];
      if (nf >= 1 && nf <= MAX_FINGER_SERVOS) cfg.numFingers = nf;
    }
    if (doc.containsKey("air_pca")) cfg.airflowPcaChannel = doc["air_pca"];
    if (doc.containsKey("angle_open")) cfg.fingerAngleOpen = doc["angle_open"];
    if (doc.containsKey("half_hole_pct")) cfg.halfHolePercent = doc["half_hole_pct"];
    if (doc.containsKey("embouchure")) {
      strncpy(cfg.embouchure, doc["embouchure"] | "trav", sizeof(cfg.embouchure) - 1);
      cfg.embouchure[sizeof(cfg.embouchure) - 1] = '\0';
    }

    // --- Scalaires ---
    if (doc.containsKey("midi_ch")) cfg.midiChannel = doc["midi_ch"];
    if (doc.containsKey("servo_delay")) cfg.servoToSolenoidDelayMs = doc["servo_delay"];
    if (doc.containsKey("valve_interval")) cfg.minNoteIntervalForValveCloseMs = doc["valve_interval"];
    if (doc.containsKey("min_note_dur")) cfg.minNoteDurationMs = doc["min_note_dur"];
    if (doc.containsKey("air_off")) cfg.servoAirflowOff = doc["air_off"];
    if (doc.containsKey("air_min")) cfg.servoAirflowMin = doc["air_min"];
    if (doc.containsKey("air_max")) cfg.servoAirflowMax = doc["air_max"];
    if (doc.containsKey("vib_freq")) cfg.vibratoFrequencyHz = doc["vib_freq"];
    if (doc.containsKey("vib_amp")) cfg.vibratoMaxAmplitudeDeg = doc["vib_amp"];
    if (doc.containsKey("cc_vol")) cfg.ccVolumeDefault = doc["cc_vol"];
    if (doc.containsKey("cc_expr")) cfg.ccExpressionDefault = doc["cc_expr"];
    if (doc.containsKey("cc_mod")) cfg.ccModulationDefault = doc["cc_mod"];
    if (doc.containsKey("cc_breath")) cfg.ccBreathDefault = doc["cc_breath"];
    if (doc.containsKey("cc_bright")) cfg.ccBrightnessDefault = doc["cc_bright"];
    if (doc.containsKey("cc2_on")) cfg.cc2Enabled = doc["cc2_on"].as<bool>();
    if (doc.containsKey("cc2_thr")) cfg.cc2SilenceThreshold = doc["cc2_thr"];
    if (doc.containsKey("cc2_curve")) cfg.cc2ResponseCurve = doc["cc2_curve"];
    if (doc.containsKey("cc2_timeout")) cfg.cc2TimeoutMs = doc["cc2_timeout"];
    if (doc.containsKey("sol_act")) cfg.solenoidPwmActivation = doc["sol_act"];
    if (doc.containsKey("sol_hold")) cfg.solenoidPwmHolding = doc["sol_hold"];
    if (doc.containsKey("sol_time")) cfg.solenoidActivationTimeMs = doc["sol_time"];
    if (doc.containsKey("time_unpower")) cfg.timeUnpower = doc["time_unpower"];
    if (doc.containsKey("hide_calib")) cfg.hideCalibration = doc["hide_calib"].as<bool>();
    if (doc.containsKey("sol_pin")) cfg.solenoidPin = doc["sol_pin"];
    if (doc.containsKey("kbd_mode")) cfg.kbdMode = doc["kbd_mode"];
    if (doc.containsKey("color")) {
      strncpy(cfg.instrumentColor, doc["color"] | "#D4B044", sizeof(cfg.instrumentColor) - 1);
      cfg.instrumentColor[sizeof(cfg.instrumentColor) - 1] = '\0';
    }
    if (doc.containsKey("air_atk_mode")) cfg.airAttackMode = doc["air_atk_mode"];
    if (doc.containsKey("air_atk_off")) cfg.airAttackOffset = doc["air_atk_off"];
    if (doc.containsKey("air_atk_ms")) cfg.airAttackMs = doc["air_atk_ms"];
    if (doc.containsKey("air_vel_resp")) cfg.airVelocityResponse = doc["air_vel_resp"];

    // Air delivery system
    if (doc.containsKey("air_mode")) cfg.airMode = doc["air_mode"];
    if (doc.containsKey("valve_servo")) cfg.valveUseServo = doc["valve_servo"].as<bool>();
    if (doc.containsKey("valve_ch")) cfg.valveServoPcaChannel = doc["valve_ch"];
    if (doc.containsKey("pump_on")) cfg.pumpEnabled = doc["pump_on"].as<bool>();
    if (doc.containsKey("pump_pin")) cfg.pumpPin = doc["pump_pin"];
    if (doc.containsKey("pump_min")) cfg.pumpMinPwm = doc["pump_min"];
    if (doc.containsKey("pump_max")) cfg.pumpMaxPwm = doc["pump_max"];
    if (doc.containsKey("res_on")) cfg.reservoirEnabled = doc["res_on"].as<bool>();
    if (doc.containsKey("sens_type")) cfg.sensorType = doc["sens_type"];
    if (doc.containsKey("sens_target")) cfg.sensorTargetMm = doc["sens_target"];
    if (doc.containsKey("sens_min")) cfg.sensorMinMm = doc["sens_min"];
    if (doc.containsKey("sens_max")) cfg.sensorMaxMm = doc["sens_max"];
    if (doc.containsKey("pid_kp")) cfg.pidKp = doc["pid_kp"];
    if (doc.containsKey("pid_ki")) cfg.pidKi = doc["pid_ki"];
    if (doc.containsKey("show_air")) cfg.showAirSystem = doc["show_air"].as<bool>();
    if (doc.containsKey("midi_limit")) {
      uint16_t ml = doc["midi_limit"];
      if (ml >= 50 && ml <= 2000) cfg.midiStorageLimitKb = ml;
    }

    if (doc.containsKey("device")) {
      strncpy(cfg.deviceName, doc["device"] | cfg.deviceName, sizeof(cfg.deviceName) - 1);
      cfg.deviceName[sizeof(cfg.deviceName) - 1] = '\0';
    }
    if (doc.containsKey("wifi_ssid")) {
      strncpy(cfg.wifiSsid, doc["wifi_ssid"] | "", sizeof(cfg.wifiSsid) - 1);
      cfg.wifiSsid[sizeof(cfg.wifiSsid) - 1] = '\0';
    }
    if (doc.containsKey("wifi_pass")) {
      strncpy(cfg.wifiPassword, doc["wifi_pass"] | "", sizeof(cfg.wifiPassword) - 1);
      cfg.wifiPassword[sizeof(cfg.wifiPassword) - 1] = '\0';
    }

    // --- Doigts (partiel) ---
    if (doc.containsKey("fingers")) {
      JsonArray fingers = doc["fingers"];
      for (int i = 0; i < cfg.numFingers && i < (int)fingers.size(); i++) {
        if (fingers[i].containsKey("ch")) cfg.fingers[i].pcaChannel = fingers[i]["ch"];
        if (fingers[i].containsKey("a")) cfg.fingers[i].closedAngle = fingers[i]["a"];
        if (fingers[i].containsKey("d")) cfg.fingers[i].direction = fingers[i]["d"];
        if (fingers[i].containsKey("th")) cfg.fingers[i].isThumbHole = (fingers[i]["th"].as<int>() != 0);
      }
    }

    // --- Notes (complete: remplace tout si present) ---
    if (doc.containsKey("notes")) {
      JsonArray notes = doc["notes"];
      int count = min((int)notes.size(), (int)MAX_NOTES);
      cfg.numNotes = count;
      for (int i = 0; i < count; i++) {
        JsonObject n = notes[i];
        cfg.notes[i].midiNote = n["midi"] | cfg.notes[i].midiNote;
        cfg.notes[i].airflowMinPercent = n["amn"] | cfg.notes[i].airflowMinPercent;
        cfg.notes[i].airflowMaxPercent = n["amx"] | cfg.notes[i].airflowMaxPercent;
        if (n.containsKey("fp")) {
          JsonArray fp = n["fp"];
          for (int f = 0; f < MAX_FINGER_SERVOS; f++) {
            cfg.notes[i].fingerPattern[f] = (f < (int)fp.size()) ? (uint8_t)fp[f].as<int>() : 0;
          }
        }
      }
    }

    // --- Notes airflow only (backward compat / step 3 save) ---
    if (doc.containsKey("notes_air")) {
      JsonArray notes = doc["notes_air"];
      for (int i = 0; i < cfg.numNotes && i < (int)notes.size(); i++) {
        if (notes[i].containsKey("amn")) cfg.notes[i].airflowMinPercent = notes[i]["amn"];
        if (notes[i].containsKey("amx")) cfg.notes[i].airflowMaxPercent = notes[i]["amx"];
      }
    }

    // Sauvegarder sur LittleFS
    bool saved = ConfigStorage::save();

    if (DEBUG) {
      Serial.println("DEBUG: WebConfigurator - Config mise a jour via web");
    }

    String resp = "{\"ok\":" + String(saved ? "true" : "false") + "}";
    request->send(200, "application/json", resp);
  }
  _configBody = "";
}

void WebConfigurator::handleApiConfigReset(AsyncWebServerRequest* request) {
  ConfigStorage::resetToDefaults();

  if (DEBUG) {
    Serial.println("DEBUG: WebConfigurator - Config reset aux defauts");
  }

  request->send(200, "application/json", "{\"ok\":true}");
}

void WebConfigurator::handleApiFactoryReset(AsyncWebServerRequest* request) {
  ConfigStorage::factoryReset();

  if (DEBUG) {
    Serial.println("DEBUG: WebConfigurator - Reset usine");
  }

  request->send(200, "application/json", "{\"ok\":true}");
}

void WebConfigurator::handleMidiUpload(AsyncWebServerRequest* request, const String& filename,
                                        size_t index, uint8_t* data, size_t len, bool final) {
  if (index == 0) {
    if (DEBUG) {
      Serial.print("DEBUG: WebConfigurator - Upload MIDI: ");
      Serial.println(filename);
    }
    _uploadSize = 0;
    // Stocker le nom original (nettoye, sans chemin)
    _uploadFileName = filename;
    int ls = _uploadFileName.lastIndexOf('/');
    if (ls >= 0) _uploadFileName = _uploadFileName.substring(ls + 1);
    int bs = _uploadFileName.lastIndexOf('\\');
    if (bs >= 0) _uploadFileName = _uploadFileName.substring(bs + 1);
    // Ecrire d'abord dans un fichier temp pour validation
    _uploadFile = LittleFS.open(MIDI_FILE_PATH, "w");
    if (!_uploadFile) {
      if (DEBUG) {
        Serial.println("ERREUR: WebConfigurator - Impossible de creer fichier temp");
      }
      return;
    }
  }

  if (_uploadFile && len > 0) {
    _uploadSize += len;
    if (_uploadSize <= MIDI_FILE_MAX_SIZE) {
      _uploadFile.write(data, len);
    }
  }

  if (final) {
    if (_uploadFile) {
      _uploadFile.close();
    }

    if (DEBUG) {
      Serial.print("DEBUG: WebConfigurator - Upload termine: ");
      Serial.print(_uploadSize);
      Serial.println(" octets");
    }
  }
}

void WebConfigurator::handleMidiUploadComplete(AsyncWebServerRequest* request) {
  if (_uploadSize > MIDI_FILE_MAX_SIZE) {
    LittleFS.remove(MIDI_FILE_PATH);
    String resp = "{\"ok\":false,\"msg\":\"Fichier trop volumineux (max " + String(MIDI_FILE_MAX_SIZE / 1024) + "KB)\"}";
    request->send(400, "application/json", resp);
    _ws.textAll("{\"t\":\"midi_error\",\"msg\":\"Fichier trop volumineux\"}");
    return;
  }

  if (_uploadSize == 0) {
    String resp = "{\"ok\":false,\"msg\":\"Aucun fichier recu\"}";
    request->send(400, "application/json", resp);
    return;
  }

  // Verifier la limite de stockage total
  size_t currentUsed = getMidiStorageUsed();
  size_t limitBytes = (size_t)cfg.midiStorageLimitKb * 1024;
  // Le fichier destination peut deja exister (ecrasement) - soustraire sa taille
  String destPath = String(MIDI_DIR) + "/" + _uploadFileName;
  size_t existingSize = 0;
  if (LittleFS.exists(destPath)) {
    File ef = LittleFS.open(destPath, "r");
    if (ef) { existingSize = ef.size(); ef.close(); }
  }
  if (currentUsed - existingSize + _uploadSize > limitBytes) {
    LittleFS.remove(MIDI_FILE_PATH);
    String resp = "{\"ok\":false,\"msg\":\"Stockage MIDI plein (" + String(currentUsed / 1024) + "/" + String(cfg.midiStorageLimitKb) + " KB)\"}";
    request->send(400, "application/json", resp);
    _ws.textAll("{\"t\":\"midi_error\",\"msg\":\"Stockage MIDI plein\"}");
    return;
  }

  // Valider le fichier MIDI en le chargeant depuis le temp
  if (_player && _player->loadFile(MIDI_FILE_PATH)) {
    // Copier le fichier temp vers /midi/<filename>
    if (!LittleFS.exists(MIDI_DIR)) {
      LittleFS.mkdir(MIDI_DIR);
    }
    // Renommer (LittleFS.rename) le fichier temp vers sa destination finale
    if (LittleFS.exists(destPath)) {
      LittleFS.remove(destPath);
    }
    LittleFS.rename(MIDI_FILE_PATH, destPath);

    String resp = "{\"ok\":true";
    resp += ",\"events\":" + String(_player->getEventCount());
    resp += ",\"duration\":" + String(_player->getDurationMs());
    resp += ",\"file\":\"" + _uploadFileName + "\"";
    resp += ",\"storage_used\":" + String(getMidiStorageUsed());
    resp += ",\"storage_limit\":" + String(limitBytes);
    resp += "}";
    request->send(200, "application/json", resp);

    String wsMsg = "{\"t\":\"midi_loaded\"";
    wsMsg += ",\"file\":\"" + _uploadFileName + "\"";
    wsMsg += ",\"events\":" + String(_player->getEventCount());
    wsMsg += ",\"duration\":" + String(_player->getDurationMs());
    wsMsg += ",\"channels\":" + String(_player->getActiveChannels());
    wsMsg += "}";
    _ws.textAll(wsMsg);
  } else {
    LittleFS.remove(MIDI_FILE_PATH);
    String resp = "{\"ok\":false,\"msg\":\"Format MIDI invalide\"}";
    request->send(400, "application/json", resp);
    _ws.textAll("{\"t\":\"midi_error\",\"msg\":\"Format MIDI invalide\"}");
  }
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
  String json = "{\"files\":[";
  File dir = LittleFS.open(MIDI_DIR);
  bool first = true;
  if (dir && dir.isDirectory()) {
    File f = dir.openNextFile();
    while (f) {
      if (!f.isDirectory()) {
        if (!first) json += ",";
        // f.name() retourne le chemin complet sur ESP32 - extraire le nom seul
        String fname = String(f.name());
        int lastSlash = fname.lastIndexOf('/');
        if (lastSlash >= 0) fname = fname.substring(lastSlash + 1);
        json += "{\"name\":\"" + fname + "\",\"size\":" + String(f.size()) + "}";
        first = false;
      }
      f = dir.openNextFile();
    }
  }
  json += "],\"used\":" + String(getMidiStorageUsed());
  json += ",\"limit\":" + String((size_t)cfg.midiStorageLimitKb * 1024);
  // Fichier actuellement charge
  if (_player && _player->isFileLoaded()) {
    json += ",\"loaded\":\"" + _player->getFileName() + "\"";
  }
  json += "}";
  request->send(200, "application/json", json);
}

void WebConfigurator::handleMidiDelete(AsyncWebServerRequest* request) {
  if (_configBody.length() == 0) {
    request->send(400, "application/json", "{\"ok\":false,\"msg\":\"Body vide\"}");
    return;
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, _configBody);
  _configBody = "";
  if (err || !doc.containsKey("file")) {
    request->send(400, "application/json", "{\"ok\":false,\"msg\":\"JSON invalide\"}");
    return;
  }
  String filename = doc["file"].as<String>();
  // Securite: extraire le nom seul (pas de path traversal)
  int lastSlash = filename.lastIndexOf('/');
  if (lastSlash >= 0) filename = filename.substring(lastSlash + 1);
  if (filename.length() == 0 || filename.indexOf("..") >= 0) {
    request->send(400, "application/json", "{\"ok\":false,\"msg\":\"Nom invalide\"}");
    return;
  }
  String path = String(MIDI_DIR) + "/" + filename;
  if (!LittleFS.exists(path)) {
    request->send(404, "application/json", "{\"ok\":false,\"msg\":\"Fichier introuvable\"}");
    return;
  }
  LittleFS.remove(path);
  if (DEBUG) {
    Serial.print("DEBUG: WebConfigurator - MIDI supprime: ");
    Serial.println(filename);
  }
  String resp = "{\"ok\":true,\"used\":" + String(getMidiStorageUsed()) + ",\"limit\":" + String((size_t)cfg.midiStorageLimitKb * 1024) + "}";
  request->send(200, "application/json", resp);
}

void WebConfigurator::handleMidiLoad(AsyncWebServerRequest* request) {
  if (_configBody.length() == 0) {
    request->send(400, "application/json", "{\"ok\":false,\"msg\":\"Body vide\"}");
    return;
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, _configBody);
  _configBody = "";
  if (err || !doc.containsKey("file")) {
    request->send(400, "application/json", "{\"ok\":false,\"msg\":\"JSON invalide\"}");
    return;
  }
  String filename = doc["file"].as<String>();
  int ls = filename.lastIndexOf('/');
  if (ls >= 0) filename = filename.substring(ls + 1);
  String path = String(MIDI_DIR) + "/" + filename;
  if (!LittleFS.exists(path)) {
    request->send(404, "application/json", "{\"ok\":false,\"msg\":\"Fichier introuvable\"}");
    return;
  }
  if (_player && _player->loadFile(path.c_str())) {
    String resp = "{\"ok\":true";
    resp += ",\"events\":" + String(_player->getEventCount());
    resp += ",\"duration\":" + String(_player->getDurationMs());
    resp += ",\"file\":\"" + _player->getFileName() + "\"";
    resp += ",\"channels\":" + String(_player->getActiveChannels());
    resp += "}";
    request->send(200, "application/json", resp);

    String wsMsg = "{\"t\":\"midi_loaded\"";
    wsMsg += ",\"file\":\"" + _player->getFileName() + "\"";
    wsMsg += ",\"events\":" + String(_player->getEventCount());
    wsMsg += ",\"duration\":" + String(_player->getDurationMs());
    wsMsg += ",\"channels\":" + String(_player->getActiveChannels());
    wsMsg += "}";
    _ws.textAll(wsMsg);
  } else {
    request->send(400, "application/json", "{\"ok\":false,\"msg\":\"Echec chargement MIDI\"}");
  }
}

// --- WebSocket ---

void WebConfigurator::onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                                 AwsEventType type, void* arg, uint8_t* data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT:
      if (DEBUG) {
        Serial.print("DEBUG: WS client connecte #");
        Serial.println(client->id());
      }
      break;

    case WS_EVT_DISCONNECT:
      if (DEBUG) {
        Serial.print("DEBUG: WS client deconnecte #");
        Serial.println(client->id());
      }
      break;

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
  String msg = String((char*)data).substring(0, len);

  if (_instrument == nullptr) return;

  // Extraire le type de message
  int tIdx = msg.indexOf("\"t\":\"");
  if (tIdx < 0) return;
  tIdx += 5;
  int tEnd = msg.indexOf("\"", tIdx);
  if (tEnd < 0) return;
  String type = msg.substring(tIdx, tEnd);

  if (type == "non") {
    int nIdx = msg.indexOf("\"n\":");
    int vIdx = msg.indexOf("\"v\":");
    if (nIdx < 0) return;
    uint8_t note = (uint8_t)msg.substring(nIdx + 4).toInt();
    uint8_t vel = _webVelocity;
    if (vIdx >= 0) {
      vel = (uint8_t)msg.substring(vIdx + 4).toInt();
    }
    _instrument->noteOn(note, vel);

  } else if (type == "nof") {
    int nIdx = msg.indexOf("\"n\":");
    if (nIdx < 0) return;
    uint8_t note = (uint8_t)msg.substring(nIdx + 4).toInt();
    _instrument->noteOff(note);

  } else if (type == "cc") {
    int cIdx = msg.indexOf("\"c\":");
    int vIdx = msg.indexOf("\"v\":");
    if (cIdx < 0 || vIdx < 0) return;
    uint8_t ccNum = (uint8_t)msg.substring(cIdx + 4).toInt();
    uint8_t ccVal = (uint8_t)msg.substring(vIdx + 4).toInt();
    _instrument->handleControlChange(ccNum, ccVal);

  } else if (type == "velocity") {
    int vIdx = msg.indexOf("\"v\":");
    if (vIdx >= 0) {
      _webVelocity = (uint8_t)msg.substring(vIdx + 4).toInt();
      if (_webVelocity < 1) _webVelocity = 1;
      if (_webVelocity > MIDI_VELOCITY_MAX) _webVelocity = MIDI_VELOCITY_MAX;
    }

  } else if (type == "air_live") {
    int vIdx = msg.indexOf("\"v\":");
    if (vIdx >= 0) {
      uint8_t pct = (uint8_t)msg.substring(vIdx + 4).toInt();
      _instrument->getAirflowCtrl().setAirflowLivePercent(pct);
    }

  } else if (type == "play") {
    if (_player) _player->play();
  } else if (type == "pause") {
    if (_player) _player->pause();
  } else if (type == "stop") {
    if (_player) _player->stop();
  } else if (type == "ch_filter") {
    if (_player) {
      int vi = msg.indexOf("\"ch\":");
      if (vi >= 0) {
        int ch = msg.substring(vi + 5).toInt();
        _player->setChannelFilter(ch > 15 ? 255 : (uint8_t)ch);
      }
    }
  } else if (type == "panic") {
    _instrument->allSoundOff();

  } else if (type == "test_finger") {
    int iIdx = msg.indexOf("\"i\":");
    int aIdx = msg.indexOf("\"a\":");
    if (iIdx >= 0 && aIdx >= 0) {
      int fi = msg.substring(iIdx + 4).toInt();
      int angle = msg.substring(aIdx + 4).toInt();
      _instrument->getFingerCtrl().testFingerAngle(fi, (uint16_t)angle);
    }

  } else if (type == "test_air") {
    int aIdx = msg.indexOf("\"a\":");
    if (aIdx >= 0) {
      int angle = msg.substring(aIdx + 4).toInt();
      _instrument->getAirflowCtrl().testAirflowAngle((uint16_t)angle);
    }

  } else if (type == "test_sol") {
    int oIdx = msg.indexOf("\"o\":");
    if (oIdx >= 0) {
      int val = msg.substring(oIdx + 4).toInt();
      _instrument->getAirflowCtrl().testSolenoid(val != 0);
    }

  } else if (type == "test_note") {
    int nIdx = msg.indexOf("\"n\":");
    if (nIdx >= 0) {
      uint8_t note = (uint8_t)msg.substring(nIdx + 4).toInt();
      _instrument->getFingerCtrl().setFingerPatternForNote(note);
      _instrument->getAirflowCtrl().setAirflowForNote(note, _webVelocity);
    }

  } else if (type == "pump_target") {
    int vIdx = msg.indexOf("\"v\":");
    if (vIdx >= 0) {
      uint8_t pct = (uint8_t)msg.substring(vIdx + 4).toInt();
      _instrument->getPressureCtrl().setTargetPercent(pct);
    }

  } else if (type == "pump_stop") {
    _instrument->getPressureCtrl().stop();

  } else if (type == "pump_enable") {
    int vIdx = msg.indexOf("\"v\":");
    if (vIdx >= 0) {
      int val = msg.substring(vIdx + 4).toInt();
      if (val == 0) {
        _instrument->getPressureCtrl().stop();
      }
      // Enable/disable is handled by stop/resume - pump resumes on next target set
    }

#if MIC_ENABLED
  } else if (type == "mic_mon") {
    int oIdx = msg.indexOf("\"on\":");
    if (oIdx >= 0) {
      int val = msg.substring(oIdx + 5).toInt();
      _micMonitorEnabled = (val != 0);
      if (_audio) _audio->setActive(_micMonitorEnabled || (_autoCal && _autoCal->isRunning()));
    }

  } else if (type == "auto_cal") {
    int mIdx = msg.indexOf("\"mode\":\"");
    if (mIdx >= 0) {
      mIdx += 8;
      int mEnd = msg.indexOf("\"", mIdx);
      String mode = msg.substring(mIdx, mEnd);

      if (mode == "air" && _autoCal && _audio && _audio->isMicDetected()) {
        _audio->setActive(true);
        _micMonitorEnabled = true;
        _autoCal->start(ACAL_MODE_AIRFLOW);
      } else if (mode == "stop") {
        if (_autoCal) _autoCal->stop();
      }
    }
#endif
  }
}

void WebConfigurator::broadcastStatus() {
  if (_ws.count() == 0) return;

  String json = "{\"t\":\"status\"";

  if (_instrument) {
    NoteSequencer& seq = _instrument->getSequencer();
    json += ",\"playing\":" + String(seq.isPlaying() ? "true" : "false");
    json += ",\"state\":" + String(seq.getState());
    json += ",\"cc7\":" + String(_instrument->getCCVolume());
    json += ",\"cc11\":" + String(_instrument->getCCExpression());
    json += ",\"cc1\":" + String(_instrument->getCCModulation());
    json += ",\"cc2\":" + String(_instrument->getCCBreath());
  }

  if (_player) {
    json += ",\"ps\":" + String(_player->getState());
    if (_player->isFileLoaded()) {
      json += ",\"pp\":" + String(_player->getProgressPercent(), 1);
      json += ",\"ppos\":" + String(_player->getPositionMs());
    }
  }

  // Air system live data
  if (_instrument && (cfg.pumpEnabled || cfg.reservoirEnabled)) {
    PressureController& pc = _instrument->getPressureCtrl();
    json += ",\"pump_pwm\":" + String(pc.getPumpPwm());
    json += ",\"res_pct\":" + String(pc.getFillPercent());
    json += ",\"res_mm\":" + String(pc.getDistanceMm());
    json += ",\"sens_ok\":" + String(pc.isSensorDetected() ? "true" : "false");
  }
  if (_instrument) {
    json += ",\"valve_open\":" + String(_instrument->getAirflowCtrl().isValveOpen() ? "true" : "false");
    json += ",\"air_angle\":" + String(_instrument->getAirflowCtrl().getAirflowAngle());
  }

  json += ",\"heap\":" + String(ESP.getFreeHeap());
  json += "}";

  _ws.textAll(json);
}

#include "WebConfigurator.h"
#include "InstrumentManager.h"
#include "FanController.h"
#include "WirelessManager.h"
#include "web_content.h"
#include "WebValueParsers.h"
#include <WiFi.h>

WebConfigurator::WebConfigurator(uint16_t port)
  : _server(port), _ws("/ws"),
    _instrument(nullptr), _player(nullptr), _wirelessManager(nullptr),
    _webVelocity(WEB_DEFAULT_VELOCITY), _lastStatusBroadcast(0), _lastWsCleanup(0),
    _uploadSize(0), _uploadError(false)
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
          aj += ",\"conf\":" + String((int)(_audio->getPitchConfidence() * 100.0f + 0.5f));
          aj += ",\"valid\":" + String(_audio->isPitchValid() ? 1 : 0);
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

    // Global-timeout safety abort: notify the UI and clear the audio monitor.
    if (_autoCal && _autoCal->takeTimeoutEvent()) {
      _ws.textAll("{\"t\":\"acal_error\",\"msg\":\"Timeout global de calibration\"}");
      _audio->setActive(_micMonitorEnabled);
    }

    // Check range finder completion
    if (_autoCal && _autoCal->isRangeFinderComplete()) {
      String dj = "{\"t\":\"rf_done\",\"ok\":";
      dj += String(_autoCal->getRangeFinderMin() >= 0 ? "true" : "false");
      if (_autoCal->getRangeFinderMin() >= 0) {
        dj += ",\"min\":" + String(_autoCal->getRangeFinderMin());
        dj += ",\"max\":" + String(_autoCal->getRangeFinderMax());
      }
      dj += "}";
      _ws.textAll(dj);
      _autoCal->stop();
    }
    // Check airflow calibration completion
    if (_autoCal && _autoCal->isComplete()) {
      // applyResults() only overwrites notes whose new calibration is valid (a
      // failed note keeps its previous configuration) and restores RAM on a
      // storage failure, reporting exactly what happened.
      AutoCalApplyResult ap = _autoCal->applyResults();
      const char* names[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
      String dj = "{\"t\":\"acal_done\"";
      dj += ",\"ok\":" + String(ap.validCount > 0 ? "true" : "false");
      dj += ",\"saved\":" + String(ap.saved ? "true" : "false");
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
        dj += ",\"minA\":" + String(minAngle);
        dj += ",\"nomA\":" + String(nomAngle);
        dj += ",\"maxA\":" + String(maxAngle) + "}";
      }
      dj += "]}";
      _ws.textAll(dj);
      _autoCal->stop();
      if (_audio) _audio->setActive(_micMonitorEnabled);
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
        _configBodyTooLarge = false;
        _configBody.reserve(min(total + 1, (size_t)CONFIG_MAX_POST_BYTES + 1));
      }
      if (total > CONFIG_MAX_POST_BYTES || _configBody.length() + len > CONFIG_MAX_POST_BYTES) {
        _configBodyTooLarge = true;
        return;
      }
      _configBody.concat((const char*)data, len);
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
        request->send(400, "application/json", "{\"ok\":false,\"msg\":\"Empty body\"}");
      }
    },
    NULL,
    [this](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
      if (index == 0) { _configBody = ""; _configBodyTooLarge = false; _configBody.reserve(min(total + 1, (size_t)CONFIG_MAX_POST_BYTES + 1)); }
      if (total > CONFIG_MAX_POST_BYTES || _configBody.length() + len > CONFIG_MAX_POST_BYTES) { _configBodyTooLarge = true; return; }
      _configBody.concat((const char*)data, len);
      if (index + len == total) {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, _configBody);
        if (err || !doc.containsKey("ssid")) {
          request->send(400, "application/json", "{\"ok\":false,\"msg\":\"Invalid JSON\"}");
        } else if (_wirelessManager) {
          const char* ssid = doc["ssid"];
          const char* pass = doc["pass"] | "";
          request->send(200, "application/json", "{\"ok\":true,\"msg\":\"Connecting...\"}");
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
      if (index == 0) { _configBody = ""; _configBodyTooLarge = false; _configBody.reserve(min(total + 1, (size_t)CONFIG_MAX_POST_BYTES + 1)); }
      if (total > CONFIG_MAX_POST_BYTES || _configBody.length() + len > CONFIG_MAX_POST_BYTES) { _configBodyTooLarge = true; return; }
      _configBody.concat((const char*)data, len);
    }
  );

  // MIDI file load (select for playback)
  _server.on("/api/midi/load", HTTP_POST,
    [this](AsyncWebServerRequest* request) {
      handleMidiLoad(request);
    },
    NULL,
    [this](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
      if (index == 0) { _configBody = ""; _configBodyTooLarge = false; _configBody.reserve(min(total + 1, (size_t)CONFIG_MAX_POST_BYTES + 1)); }
      if (total > CONFIG_MAX_POST_BYTES || _configBody.length() + len > CONFIG_MAX_POST_BYTES) { _configBodyTooLarge = true; return; }
      _configBody.concat((const char*)data, len);
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
      handleMidiUpload(request, filename, index, data, len, final);
    }
  );

  // Safe restart API
  _server.on("/api/restart", HTTP_POST, [this](AsyncWebServerRequest* request) {
    if (_instrument) { _instrument->allSoundOff(); }
    request->send(200, "application/json", "{\"ok\":true,\"msg\":\"Restarting\"}");
    delay(50);
    ESP.restart();
  });

  // Hardware diagnostics API
  _server.on("/api/diagnostics", HTTP_GET, [this](AsyncWebServerRequest* request) {
    handleApiDiagnostics(request);
  });
  _server.on("/api/diagnostics/run", HTTP_POST, [this](AsyncWebServerRequest* request) {
    handleApiDiagnostics(request);
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
  AsyncWebServerResponse* response = request->beginResponse_P(200, "text/html", (const uint8_t*)WEB_HTML_CONTENT, sizeof(WEB_HTML_CONTENT) - 1);
  request->send(response);
}

void WebConfigurator::handleApiStatus(AsyncWebServerRequest* request) {
  JsonDocument doc;
  doc["mode"] = _wirelessManager ? _wirelessManager->getStatusText() : "N/A";
  doc["connected"] = _wirelessManager ? _wirelessManager->isMidiConnected() : false;
  doc["uptime"] = millis() / 1000;

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
  json += ",\"device\":\"" + String(cfg.deviceName) + "\"";
  json += ",\"wifi_ssid\":\"" + String(cfg.wifiSsid) + "\"";
  json += ",\"time_unpower\":" + String(cfg.timeUnpower);
  json += ",\"hide_calib\":" + String(cfg.hideCalibration ? "true" : "false");
  json += ",\"hide_air\":" + String(cfg.hideAir ? "true" : "false");
  json += ",\"sol_pin\":" + String(cfg.solenoidPin);
  json += ",\"kbd_mode\":" + String(cfg.kbdMode);
  json += ",\"color\":\"" + String(cfg.instrumentColor) + "\"";
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
  json += ",\"res_format\":\"" + String(cfg.resFormat) + "\"";
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
  request->send(200, "application/json", json);
}

void WebConfigurator::handleApiConfigFinalize(AsyncWebServerRequest* request) {
  if (_configBody.length() == 0) {
    request->send(400, "application/json", "{\"ok\":false,\"msg\":\"Empty body\"}");
    return;
  }

  {
    if (_configBody.length() > CONFIG_MAX_POST_BYTES) {
      request->send(413, "application/json", "{\"ok\":false,\"msg\":\"Config body too large\"}");
      _configBody = "";
      return;
    }
    RuntimeConfig previousConfig = cfg;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, _configBody);

    if (err) {
      request->send(400, "application/json", "{\"ok\":false,\"msg\":\"Invalid JSON\"}");
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
    if (doc.containsKey("smidi_on")) cfg.serialMidiEnabled = doc["smidi_on"].as<bool>();
    if (doc.containsKey("smidi_rx")) {
      uint8_t pin = doc["smidi_rx"];
      // Valider que le GPIO est dans la liste autorisee
      const uint8_t validPins[] = {16,17,18,19,23,25,26,27,33,34,35,36,39};
      for (uint8_t i = 0; i < sizeof(validPins); i++) {
        if (pin == validPins[i]) { cfg.serialMidiRxPin = pin; break; }
      }
    }
    if (doc.containsKey("servo_delay")) cfg.servoToSolenoidDelayMs = doc["servo_delay"];
    if (doc.containsKey("valve_interval")) cfg.minNoteIntervalForValveCloseMs = doc["valve_interval"];
    if (doc.containsKey("min_note_dur")) cfg.minNoteDurationMs = doc["min_note_dur"];
    if (doc.containsKey("air_off")) cfg.servoAirflowOff = doc["air_off"];
    if (doc.containsKey("air_min")) cfg.servoAirflowMin = doc["air_min"];
    if (doc.containsKey("air_max")) cfg.servoAirflowMax = doc["air_max"];
    if (doc.containsKey("ang_pca")) cfg.angleServoPcaChannel = doc["ang_pca"];  // legacy migration
    if (doc.containsKey("ang_off")) cfg.servoAngleOff = doc["ang_off"];
    if (doc.containsKey("ang_min")) cfg.servoAngleMin = doc["ang_min"];
    if (doc.containsKey("ang_max")) cfg.servoAngleMax = doc["ang_max"];
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
    if (doc.containsKey("hide_air")) cfg.hideAir = doc["hide_air"].as<bool>();
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

    // Air delivery system (modulaire)
    if (doc.containsKey("air_mode")) {
      uint8_t am = doc["air_mode"];
      // Retro-compat: ancien mode 6 -> mode 5 + endstop meca
      if (am == 6) { am = AIR_MODE_PUMP_RESERVOIR; cfg.sensorType = SENSOR_TYPE_ENDSTOP_MECH; }
      cfg.airMode = am;
    }
    if (doc.containsKey("valve_type")) cfg.valveType = doc["valve_type"];
    // Retro-compat: ancien champ valve_servo
    if (doc.containsKey("valve_servo") && !doc.containsKey("valve_type")) {
      cfg.valveType = doc["valve_servo"].as<bool>() ? 1 : 0;
    }
    if (doc.containsKey("valve_ch")) cfg.valveServoPcaChannel = doc["valve_ch"];
    if (doc.containsKey("angle_on")) cfg.angleServoEnabled = doc["angle_on"].as<bool>();
    if (doc.containsKey("angle_ch")) cfg.angleServoPcaChannel = doc["angle_ch"];
    if (doc.containsKey("vlv_close")) cfg.valveServoCloseAngle = doc["vlv_close"];
    if (doc.containsKey("vlv_open")) cfg.valveServoOpenAngle = doc["vlv_open"];
    // vlv_dir is intentionally ignored; close/open angles fully define valve direction.
    if (!doc.containsKey("valve_interval") && doc.containsKey("sol_inter")) cfg.minNoteIntervalForValveCloseMs = doc["sol_inter"];
    cfg.solenoidInterNoteMs = cfg.minNoteIntervalForValveCloseMs;
    if (doc.containsKey("motor_type")) cfg.motorType = doc["motor_type"];
    if (doc.containsKey("fan_pin")) cfg.fanPin = doc["fan_pin"];
    if (doc.containsKey("fan_min")) cfg.fanMinPwm = doc["fan_min"];
    if (doc.containsKey("fan_max")) cfg.fanMaxPwm = doc["fan_max"];
    if (doc.containsKey("fan_idle_pct")) cfg.fanIdlePercent = doc["fan_idle_pct"];
    if (doc.containsKey("fan_idle_timeout")) cfg.fanIdleTimeoutMs = doc["fan_idle_timeout"];
    if (doc.containsKey("fan_default_pct")) cfg.fanDefaultPercent = doc["fan_default_pct"];
    if (doc.containsKey("fan_note_max_pct")) cfg.fanMaxNotePercent = doc["fan_note_max_pct"];
    if (doc.containsKey("fan_follow_air")) cfg.fanFollowAirflow = doc["fan_follow_air"].as<bool>();
    if (doc.containsKey("num_pumps")) {
      uint8_t np = doc["num_pumps"];
      if (np >= 1 && np <= MAX_PUMPS) cfg.numPumps = np;
    }
    if (doc.containsKey("pump_pins")) {
      JsonArray pp = doc["pump_pins"];
      for (int i = 0; i < MAX_PUMPS && i < (int)pp.size(); i++) cfg.pumpPins[i] = pp[i];
    }
    if (doc.containsKey("pump_mins")) {
      JsonArray pm = doc["pump_mins"];
      for (int i = 0; i < MAX_PUMPS && i < (int)pm.size(); i++) cfg.pumpMinPwm[i] = pm[i];
    }
    if (doc.containsKey("pump_maxs")) {
      JsonArray px = doc["pump_maxs"];
      for (int i = 0; i < MAX_PUMPS && i < (int)px.size(); i++) cfg.pumpMaxPwm[i] = px[i];
    }
    // Retro-compat: ancien champ pump_pin unique
    if (doc.containsKey("pump_pin") && !doc.containsKey("pump_pins")) {
      cfg.pumpPins[0] = doc["pump_pin"];
    }
    if (doc.containsKey("pump_min") && !doc.containsKey("pump_mins")) {
      cfg.pumpMinPwm[0] = doc["pump_min"];
    }
    if (doc.containsKey("pump_max") && !doc.containsKey("pump_maxs")) {
      cfg.pumpMaxPwm[0] = doc["pump_max"];
    }
    if (doc.containsKey("pump_cascade")) {
      uint8_t v = doc["pump_cascade"];
      cfg.pumpCascadeThreshold = (v <= 100) ? v : 100;
    }
    if (doc.containsKey("pump_stagger")) cfg.pumpStaggerMs = doc["pump_stagger"];
    if (doc.containsKey("pump_idle_pct")) cfg.pumpDirectIdlePercent = doc["pump_idle_pct"];
    if (doc.containsKey("pump_direct_max_pct")) cfg.pumpDirectMaxPercent = doc["pump_direct_max_pct"];
    if (doc.containsKey("pump_follow_air")) cfg.pumpFollowAirflow = doc["pump_follow_air"].as<bool>();
    if (doc.containsKey("res_target_pct")) cfg.reservoirTargetPercent = doc["res_target_pct"];
    if (doc.containsKey("res_autostart")) cfg.reservoirAutoStart = doc["res_autostart"].as<bool>();
    if (doc.containsKey("bb_hyst")) {
      uint8_t v = doc["bb_hyst"];
      cfg.bangbangHysteresis = (v <= 50) ? v : 50;
    }
    if (doc.containsKey("sens_type")) cfg.sensorType = doc["sens_type"];
    if (doc.containsKey("sens_target")) cfg.sensorTargetMm = doc["sens_target"];
    if (doc.containsKey("sens_min")) cfg.sensorMinMm = doc["sens_min"];
    if (doc.containsKey("sens_max")) cfg.sensorMaxMm = doc["sens_max"];
    if (doc.containsKey("pid_kp")) cfg.pidKp = doc["pid_kp"];
    if (doc.containsKey("pid_ki")) cfg.pidKi = doc["pid_ki"];
    if (doc.containsKey("endstop_pin")) cfg.endstopPin = doc["endstop_pin"];
    if (doc.containsKey("endstop_high")) cfg.endstopActiveHigh = doc["endstop_high"].as<bool>();
    if (doc.containsKey("endstop_pump_on")) cfg.endstopPumpOn = doc["endstop_pump_on"].as<bool>();
    if (doc.containsKey("hall_pin")) cfg.hallPin = doc["hall_pin"];
    if (doc.containsKey("hall_low")) cfg.hallThresholdLow = doc["hall_low"];
    if (doc.containsKey("hall_high")) cfg.hallThresholdHigh = doc["hall_high"];
    if (doc.containsKey("show_air")) cfg.showAirSystem = doc["show_air"].as<bool>();
    if (doc.containsKey("res_format")) {
      const char* rf = doc["res_format"];
      if (rf) strlcpy(cfg.resFormat, rf, sizeof(cfg.resFormat));
    }
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
        if (fingers[i].containsKey("hp")) cfg.fingers[i].halfPercent = constrain(fingers[i]["hp"].as<int>(), 0, 100);
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
        // Nominal: use the provided value, else derive it from min/max so older
        // clients (and stale values) never violate min <= nominal <= max.
        if (n.containsKey("anm")) {
          cfg.notes[i].airflowNominalPercent = n["anm"];
        } else {
          uint8_t mn = cfg.notes[i].airflowMinPercent, mx = cfg.notes[i].airflowMaxPercent;
          cfg.notes[i].airflowNominalPercent = (mx >= mn) ? (uint8_t)(mn + (2 * (mx - mn)) / 5) : mn;
        }
        cfg.notes[i].anglePercent = n["ang"] | cfg.notes[i].anglePercent;
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
        if (notes[i].containsKey("anm")) {
          cfg.notes[i].airflowNominalPercent = notes[i]["anm"];
        } else if (notes[i].containsKey("amn") || notes[i].containsKey("amx")) {
          // Recompute nominal when the range changed but no explicit nominal was sent.
          uint8_t mn = cfg.notes[i].airflowMinPercent, mx = cfg.notes[i].airflowMaxPercent;
          cfg.notes[i].airflowNominalPercent = (mx >= mn) ? (uint8_t)(mn + (2 * (mx - mn)) / 5) : mn;
        }
        if (notes[i].containsKey("ang")) cfg.notes[i].anglePercent = notes[i]["ang"];
      }
    }

    // --- Notes angle only (step 3 partial save, trav) ---
    if (doc.containsKey("notes_ang")) {
      JsonArray notes = doc["notes_ang"];
      for (int i = 0; i < cfg.numNotes && i < (int)notes.size(); i++) {
        if (notes[i].containsKey("ang")) cfg.notes[i].anglePercent = notes[i]["ang"];
      }
    }

    ConfigValidationResult validation = validateAndNormalizeConfig(cfg, &previousConfig);
    if (!validation.valid) {
      cfg = previousConfig;
      JsonDocument errDoc;
      errDoc["ok"] = false;
      errDoc["msg"] = "Invalid configuration";
      errDoc["error"] = validation.error;
      String errJson;
      serializeJson(errDoc, errJson);
      request->send(400, "application/json", errJson);
      _configBody = "";
      return;
    }

    ConfigApplyResult applyResult{false, validation.restartRequired, "", ""};
    if (_instrument) {
      applyResult = _instrument->applyRuntimeConfig(previousConfig, cfg);
    } else {
      applyResult.applied = !validation.restartRequired;
    }

    // Sauvegarder sur LittleFS
    bool saved = ConfigStorage::save();

    if (DEBUG) {
      Serial.println("DEBUG: WebConfigurator - Config mise a jour via web");
    }

    JsonDocument respDoc;
    respDoc["ok"] = saved;
    respDoc["saved"] = saved;
    respDoc["applied"] = applyResult.applied;
    respDoc["restart_required"] = validation.restartRequired || applyResult.restartRequired;
    respDoc["corrected"] = validation.corrected;
    JsonArray reinitialized = respDoc["reinitialized"].to<JsonArray>();
    int start = 0;
    while (start < (int)applyResult.reinitialized.length()) {
      int comma = applyResult.reinitialized.indexOf(',', start);
      if (comma < 0) comma = applyResult.reinitialized.length();
      if (comma > start) reinitialized.add(applyResult.reinitialized.substring(start, comma));
      start = comma + 1;
    }
    JsonArray warnings = respDoc["warnings"].to<JsonArray>();
    if (validation.warnings.length() > 0) warnings.add(validation.warnings);
    if (applyResult.warnings.length() > 0) warnings.add(applyResult.warnings);
    String resp;
    serializeJson(respDoc, resp);
    request->send(saved ? 200 : 500, "application/json", resp);
  }
  _configBody = "";
  _configBodyTooLarge = false;
}

void WebConfigurator::handleApiDiagnostics(AsyncWebServerRequest* request) {
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

  String loadMsg;
  switch (ConfigStorage::lastLoadStatus()) {
    case CONFIG_DEFAULTS: loadMsg = "defaults active"; break;
    case CONFIG_LOADED: loadMsg = "configuration loaded"; break;
    case CONFIG_INVALID_FALLBACK: loadMsg = "invalid /config.json, safe defaults active: " + ConfigStorage::lastLoadError(); break;
    case CONFIG_STORAGE_ERROR: loadMsg = "storage/JSON error, safe defaults active: " + ConfigStorage::lastLoadError(); break;
  }
  addCheck("config", validation.valid ? "ok" : "error", validation.valid ? "Runtime configuration is valid" : validation.error);
  addCheck("boot_config", (ConfigStorage::lastLoadStatus() == CONFIG_INVALID_FALLBACK || ConfigStorage::lastLoadStatus() == CONFIG_STORAGE_ERROR) ? "error" : "ok", loadMsg);
  addCheck("littlefs", LittleFS.totalBytes() > 0 ? "ok" : "error", String(LittleFS.usedBytes()) + "/" + String(LittleFS.totalBytes()) + " bytes used");
  addCheck("config_file", LittleFS.exists(CONFIG_FILE_PATH) ? "ok" : "warning", LittleFS.exists(CONFIG_FILE_PATH) ? "Configuration file is present" : "Using defaults; /config.json is absent");
  addCheck("pca0", "warning", "Runtime probe requires hardware; expected address 0x40");
  bool needPca1 = cfg.airflowPcaChannel >= 16 || cfg.valveServoPcaChannel >= 16 || cfg.angleServoPcaChannel >= 16;
  for (uint8_t i = 0; i < cfg.numFingers; i++) if (cfg.fingers[i].pcaChannel >= 16) needPca1 = true;
  addCheck("pca1", needPca1 ? "warning" : "ok", needPca1 ? "Second PCA9685 is required by configured channels; hardware probe requires device" : "Second PCA9685 not required");
#if MIC_ENABLED
  addCheck("microphone", (_audio && _audio->isMicDetected()) ? "ok" : "warning", (_audio && _audio->isMicDetected()) ? "Microphone detected" : "Microphone not detected or not initialized");
#else
  addCheck("microphone", "warning", "Microphone support disabled at compile time");
#endif
  addCheck("restart", "ok", "Restart-required state is reported by POST /api/config responses");
  addCheck("heap", "ok", String(ESP.getFreeHeap()) + " bytes free heap");
  doc["ok"] = validation.valid && ConfigStorage::lastLoadStatus() != CONFIG_INVALID_FALLBACK && ConfigStorage::lastLoadStatus() != CONFIG_STORAGE_ERROR;
  doc["config_load_status"] = (int)ConfigStorage::lastLoadStatus();
  doc["config_load_error"] = ConfigStorage::lastLoadError();
  String out;
  serializeJson(doc, out);
  request->send(200, "application/json", out);
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
    _uploadError = false;
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
        Serial.println("ERREUR: WebConfigurator - Unable to create temp file");
      }
      _uploadError = true;
      return;
    }
  }

  if (len > 0) {
    _uploadSize += len;
    if (_uploadFile && _uploadSize <= MIDI_FILE_MAX_SIZE) {
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
  // Verifier si une erreur est survenue pendant l'upload (ex: echec creation fichier temp)
  if (_uploadError) {
    String resp = "{\"ok\":false,\"msg\":\"Temporary file write error\"}";
    request->send(500, "application/json", resp);
    _ws.textAll("{\"t\":\"midi_error\",\"msg\":\"Temporary file write error\"}");
    return;
  }

  if (_uploadSize > MIDI_FILE_MAX_SIZE) {
    LittleFS.remove(MIDI_FILE_PATH);
    String resp = "{\"ok\":false,\"msg\":\"File too large (max " + String(MIDI_FILE_MAX_SIZE / 1024) + "KB)\"}";
    request->send(400, "application/json", resp);
    _ws.textAll("{\"t\":\"midi_error\",\"msg\":\"File too large\"}");
    return;
  }

  if (_uploadSize == 0) {
    String resp = "{\"ok\":false,\"msg\":\"No file received\"}";
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
    String resp = "{\"ok\":false,\"msg\":\"MIDI storage full (" + String(currentUsed / 1024) + "/" + String(cfg.midiStorageLimitKb) + " KB)\"}";
    request->send(400, "application/json", resp);
    _ws.textAll("{\"t\":\"midi_error\",\"msg\":\"MIDI storage full\"}");
    return;
  }

  // S'assurer que le repertoire MIDI existe
  if (!LittleFS.exists(MIDI_DIR)) {
    LittleFS.mkdir(MIDI_DIR);
  }

  // Deplacer le fichier temp vers sa destination finale
  if (LittleFS.exists(destPath)) {
    LittleFS.remove(destPath);
  }
  bool moved = LittleFS.rename(MIDI_FILE_PATH, destPath);

  // Fallback: copie manuelle si rename echoue (certaines versions ESP32 LittleFS)
  if (!moved) {
    if (DEBUG) {
      Serial.println("DEBUG: WebConfigurator - rename echoue, copie manuelle...");
    }
    File src = LittleFS.open(MIDI_FILE_PATH, "r");
    File dst = LittleFS.open(destPath, "w");
    if (src && dst) {
      uint8_t buf[512];
      while (src.available()) {
        size_t n = src.read(buf, sizeof(buf));
        dst.write(buf, n);
      }
      dst.close();
      src.close();
      LittleFS.remove(MIDI_FILE_PATH);
      moved = true;
    } else {
      if (src) src.close();
      if (dst) dst.close();
    }
  }

  if (!moved) {
    LittleFS.remove(MIDI_FILE_PATH);
    String resp = "{\"ok\":false,\"msg\":\"MIDI file storage error\"}";
    request->send(500, "application/json", resp);
    _ws.textAll("{\"t\":\"midi_error\",\"msg\":\"File storage error\"}");
    return;
  }

  // Charger le fichier MIDI depuis sa destination finale (pas le temp)
  if (_player && _player->loadFile(destPath.c_str())) {
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
    LittleFS.remove(destPath);
    String resp = "{\"ok\":false,\"msg\":\"Invalid MIDI format\"}";
    request->send(400, "application/json", resp);
    _ws.textAll("{\"t\":\"midi_error\",\"msg\":\"Invalid MIDI format\"}");
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
  if (_configBodyTooLarge) {
    request->send(413, "application/json", "{\"ok\":false,\"msg\":\"Body too large\"}");
    _configBody = ""; _configBodyTooLarge = false;
    return;
  }
  if (_configBody.length() == 0) {
    request->send(400, "application/json", "{\"ok\":false,\"msg\":\"Empty body\"}");
    return;
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, _configBody);
  _configBody = ""; _configBodyTooLarge = false;
  if (err || !doc.containsKey("file")) {
    request->send(400, "application/json", "{\"ok\":false,\"msg\":\"Invalid JSON\"}");
    return;
  }
  String filename = doc["file"].as<String>();
  // Securite: extraire le nom seul (pas de path traversal)
  int lastSlash = filename.lastIndexOf('/');
  if (lastSlash >= 0) filename = filename.substring(lastSlash + 1);
  if (filename.length() == 0 || filename.indexOf("..") >= 0) {
    request->send(400, "application/json", "{\"ok\":false,\"msg\":\"Invalid name\"}");
    return;
  }
  String path = String(MIDI_DIR) + "/" + filename;
  if (!LittleFS.exists(path)) {
    request->send(404, "application/json", "{\"ok\":false,\"msg\":\"File not found\"}");
    return;
  }
  LittleFS.remove(path);
  if (DEBUG) {
    Serial.print("DEBUG: WebConfigurator - MIDI deleted: ");
    Serial.println(filename);
  }
  String resp = "{\"ok\":true,\"used\":" + String(getMidiStorageUsed()) + ",\"limit\":" + String((size_t)cfg.midiStorageLimitKb * 1024) + "}";
  request->send(200, "application/json", resp);
}

void WebConfigurator::handleMidiLoad(AsyncWebServerRequest* request) {
  if (_configBodyTooLarge) {
    request->send(413, "application/json", "{\"ok\":false,\"msg\":\"Body too large\"}");
    _configBody = ""; _configBodyTooLarge = false;
    return;
  }
  if (_configBody.length() == 0) {
    request->send(400, "application/json", "{\"ok\":false,\"msg\":\"Empty body\"}");
    return;
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, _configBody);
  _configBody = ""; _configBodyTooLarge = false;
  if (err || !doc.containsKey("file")) {
    request->send(400, "application/json", "{\"ok\":false,\"msg\":\"Invalid JSON\"}");
    return;
  }
  String filename = doc["file"].as<String>();
  int ls = filename.lastIndexOf('/');
  if (ls >= 0) filename = filename.substring(ls + 1);
  String path = String(MIDI_DIR) + "/" + filename;
  if (!LittleFS.exists(path)) {
    request->send(404, "application/json", "{\"ok\":false,\"msg\":\"File not found\"}");
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
    request->send(400, "application/json", "{\"ok\":false,\"msg\":\"MIDI load failed\"}");
  }
}

// --- WebSocket ---

void WebConfigurator::onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                                 AwsEventType type, void* arg, uint8_t* data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT:
      if (DEBUG) {
        Serial.print("DEBUG: WS client connected #");
        Serial.println(client->id());
      }
      break;

    case WS_EVT_DISCONNECT:
#if MIC_ENABLED
      // A running microphone calibration is an active actuator test: stop it and
      // return the hardware to a safe state when the controlling client leaves.
      if (_autoCal && _autoCal->isRunning()) {
        _autoCal->stop();
        if (_audio) _audio->setActive(_micMonitorEnabled);
      }
#endif
      if (_instrument) { _instrument->allSoundOff(); }
      if (DEBUG) {
        Serial.print("DEBUG: WS client disconnected #");
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
  if (_instrument == nullptr) return;
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

  if (strcmp(type, "non") == 0) {
    if (!hasInt("n")) return;
    uint8_t note = getMidi7Bit(doc, "n", 0);
    uint8_t vel = (uint8_t)constrain(doc["v"] | _webVelocity, 1, MIDI_VELOCITY_MAX);
    _instrument->noteOn(note, vel);
  } else if (strcmp(type, "nof") == 0) {
    if (!hasInt("n")) return;
    _instrument->noteOff(getMidi7Bit(doc, "n", 0));
  } else if (strcmp(type, "cc") == 0) {
    if (!hasInt("c") || !hasInt("v")) return;
    _instrument->handleControlChange((uint8_t)getMidi7Bit(doc, "c", 0), getMidi7Bit(doc, "v", 0));
  } else if (strcmp(type, "velocity") == 0) {
    _webVelocity = (uint8_t)constrain(doc["v"] | _webVelocity, 1, MIDI_VELOCITY_MAX);
  } else if (strcmp(type, "air_live") == 0) {
    _instrument->getAirflowCtrl().setAirflowLivePercent(getPercent(doc, "v", 0));
  } else if (strcmp(type, "play") == 0) {
    if (_player) _player->play();
  } else if (strcmp(type, "pause") == 0) {
    if (_player) _player->pause();
  } else if (strcmp(type, "stop") == 0) {
    if (_player) _player->stop();
    _instrument->allSoundOff();
  } else if (strcmp(type, "ch_filter") == 0) {
    if (_player) {
      int ch = doc["ch"] | 255;
      _player->setChannelFilter(ch > 15 ? 255 : (uint8_t)ch);
    }
  } else if (strcmp(type, "panic") == 0) {
    _instrument->allSoundOff();
  } else if (strcmp(type, "test_finger") == 0) {
    int fi = doc["i"] | -1;
    int angle = getServoAngle(doc, "a", 0);
    if (fi >= 0 && fi < cfg.numFingers) _instrument->getFingerCtrl().testFingerAngle(fi, (uint16_t)angle);
  } else if (strcmp(type, "test_air") == 0) {
    _instrument->getAirflowCtrl().testAirflowAngle(getServoAngle(doc, "a", 0));
  } else if (strcmp(type, "test_angle") == 0) {
    _instrument->getAirflowCtrl().testAngleServoAngle(getServoAngle(doc, "a", 0));
  } else if (strcmp(type, "angle_live") == 0) {
    _instrument->getAirflowCtrl().setAngleLivePercent(getPercent(doc, "v", 0));
  } else if (strcmp(type, "test_sol") == 0) {
    _instrument->getAirflowCtrl().testSolenoid((doc["o"] | 0) != 0);
  } else if (strcmp(type, "test_note") == 0) {
    uint8_t note = getMidi7Bit(doc, "n", 0);
    if (_instrument->isNotePlayable(note)) {
      _instrument->getFingerCtrl().setFingerPatternForNote(note);
      _instrument->getAirflowCtrl().setAirflowForNote(note, _webVelocity);
    }
  } else if (strcmp(type, "pump_target") == 0) {
    _instrument->getPressureCtrl().setTargetPercent(getPercent(doc, "v", 0));
  } else if (strcmp(type, "pump_stop") == 0) {
    _instrument->getPressureCtrl().stop();
  } else if (strcmp(type, "fan_target") == 0) {
    _instrument->getFanCtrl().setSpeed(getPercent(doc, "v", 0));
  } else if (strcmp(type, "fan_stop") == 0) {
    _instrument->getFanCtrl().stop();
#if MIC_ENABLED
  } else if (strcmp(type, "mic_mon") == 0) {
    _micMonitorEnabled = ((doc["on"] | 0) != 0);
    if (_audio) _audio->setActive(_micMonitorEnabled || (_autoCal && _autoCal->isRunning()));
  } else if (strcmp(type, "auto_cal") == 0) {
    const char* mode = doc["mode"] | "";
    if (strcmp(mode, "air") == 0 && _autoCal && _audio && _audio->isMicDetected()) {
      _audio->setActive(true); _micMonitorEnabled = true; _autoCal->start(ACAL_MODE_AIRFLOW);
    } else if (strcmp(mode, "range") == 0 && _autoCal && _audio && _audio->isMicDetected()) {
      _audio->setActive(true); _micMonitorEnabled = true; _autoCal->start(ACAL_MODE_RANGE_FIND);
    } else if (strcmp(mode, "stop") == 0) {
      if (_autoCal) _autoCal->stop();
      if (_audio) _audio->setActive(_micMonitorEnabled);
    } else if (strcmp(mode, "apply_range") == 0) {
      if (_autoCal && _autoCal->isRangeFinderComplete()) {
        _autoCal->applyRangeResults();
        String rj = "{\"t\":\"rf_applied\",\"min\":" + String(cfg.servoAirflowMin) +
                    ",\"max\":" + String(cfg.servoAirflowMax) + "}";
        _ws.textAll(rj);
      }
    }
  } else if (strcmp(type, "auto_stop") == 0) {
    if (_autoCal) _autoCal->stop();
    if (_audio) _audio->setActive(_micMonitorEnabled);
#endif
  } else {
    client->text("{\"t\":\"error\",\"msg\":\"Unknown message type\"}");
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
  if (_instrument && cfg.airMode >= AIR_MODE_PUMP_VALVE) {
    PressureController& pc = _instrument->getPressureCtrl();
    json += ",\"pump_pwm\":" + String(pc.getPumpPwm());
    json += ",\"res_pct\":" + String(pc.getFillPercent());
    json += ",\"res_mm\":" + String(pc.getDistanceMm());
    json += ",\"hall_val\":" + String(pc.getHallValue());
    json += ",\"endstop_st\":" + String(pc.isEndstopActive() ? "true" : "false");
    json += ",\"sens_ok\":" + String(pc.isSensorDetected() ? "true" : "false");
    json += ",\"active_pumps\":" + String(pc.getActivePumpCount());
    json += ",\"bb_on\":" + String(pc.isBangbangOn() ? "true" : "false");
  }
  // Fan live data
  if (_instrument && cfg.airMode == AIR_MODE_FAN_SERVO) {
    FanController& fc = _instrument->getFanCtrl();
    json += ",\"fan_pwm\":" + String(fc.getPwm());
    json += ",\"fan_speed\":" + String(fc.getSpeed());
    json += ",\"fan_ready\":" + String(fc.isReady() ? "true" : "false");
    json += ",\"fan_idle\":" + String(fc.isIdle() ? "true" : "false");
  }
  if (_instrument) {
    json += ",\"valve_open\":" + String(_instrument->getAirflowCtrl().isValveOpen() ? "true" : "false");
    json += ",\"air_angle\":" + String(_instrument->getAirflowCtrl().getAirflowAngle());
    if (strcmp(cfg.embouchure, "trav") == 0) {
      json += ",\"ang_angle\":" + String(_instrument->getAirflowCtrl().getAngleServoAngle());
    }
  }

  json += ",\"heap\":" + String(ESP.getFreeHeap());
  json += "}";

  _ws.textAll(json);
}

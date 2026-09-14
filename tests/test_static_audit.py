from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]

def read(path):
    return (ROOT / path).read_text(encoding='utf-8')

def test_validation_entry_points_present():
    assert 'validateAndNormalizeConfig(RuntimeConfig& config' in read('Servo_flute_ESP32/ConfigStorage.h')
    assert 'validateAndNormalizeConfig(RuntimeConfig& config' in read('Servo_flute_ESP32/ConfigValidator.cpp')
    assert 'validateAndNormalizeConfig(cfg' in read('Servo_flute_ESP32/Servo_flute_ESP32.ino')
    assert 'validateAndNormalizeConfig(cfg, &previousConfig)' in read('Servo_flute_ESP32/WebConfigurator.cpp')

def test_websocket_uses_arduinojson_not_indexof_type_parsing():
    src = read('Servo_flute_ESP32/WebConfigurator.cpp')
    fn = src.split('void WebConfigurator::processWsMessage', 1)[1].split('void WebConfigurator::broadcastStatus', 1)[0]
    assert 'deserializeJson(doc, data, len)' in fn
    assert 'indexOf("\\"t' not in fn
    assert 'len > 512' in fn

def test_legacy_angle_migration_and_no_valve_dir_save():
    src = read('Servo_flute_ESP32/ConfigStorage.cpp')
    assert 'doc["angle_ch"] = cfg.angleServoPcaChannel' in src
    save_fn = src.split('bool ConfigStorage::save()', 1)[1]
    assert 'doc["ang_pca"]' not in save_fn
    assert 'doc["vlv_dir"]' not in src

def test_pressure_zero_division_guards():
    src = read('Servo_flute_ESP32/PressureController.cpp')
    assert 'hallSpan == 0' in src
    assert 'sensorSpan == 0' in src
    assert 'denom == 0' in src
    assert 'cfg.pumpCascadeThreshold > 99 ? 99' in src

def test_hardware_matrix_marks_not_tested():
    matrix = read('Servo_flute_ESP32/docs/HARDWARE_TEST_MATRIX.md')
    assert 'NOT TESTED — requires hardware' in matrix


def test_restart_ui_and_api_present():
    web = read('Servo_flute_ESP32/web_content.h')
    api = read('Servo_flute_ESP32/WebConfigurator.cpp')
    assert 'restart_required' in web
    assert 'restartNow()' in web
    assert '"/api/restart"' in api
    assert 'ESP.restart()' in api

def test_pressure_pid_outputs_logical_pwm_not_physical_pwm():
    src = read('Servo_flute_ESP32/PressureController.cpp')
    assert 'constrain(output * 2.55f, cfg.pumpMinPwm[0], cfg.pumpMaxPwm[0])' not in src
    assert src.count('constrain(output * 2.55f, 0, 255)') == 2
    assert 'pumpVal = cfg.pumpMinPwm[index] + (uint16_t)(cfg.pumpMaxPwm[index] - cfg.pumpMinPwm[index]) * pwm / 255' in src

def test_autocalibrator_range_find_is_canonical():
    files = ['Servo_flute_ESP32/AutoCalibrator.h','Servo_flute_ESP32/AutoCalibrator.cpp','Servo_flute_ESP32/WebConfigurator.cpp']
    joined = '\n'.join(read(f) for f in files)
    assert 'ACAL_MODE_RANGE_FIND' in joined
    assert 'ACAL_MODE_RANGE_FINDER' not in joined


def test_autocal_nominal_persistence_and_validation():
    # Nominal airflow is stored, migrated when absent, and validated 0<=min<=nominal<=max.
    cs = read('Servo_flute_ESP32/ConfigStorage.cpp')
    assert 'airflowNominalPercent' in cs
    assert 'n["anm"] = cfg.notes[i].airflowNominalPercent' in cs   # save
    assert 'containsKey("anm")' in cs                              # migration branch on load
    val = read('Servo_flute_ESP32/ConfigValidator.cpp')
    assert 'airflow nominal < min' in val
    assert 'airflow nominal > max' in val


def test_autocal_websocket_serialization_fields():
    # Progress + result messages expose the new adaptive-calibration data.
    web = read('Servo_flute_ESP32/WebConfigurator.cpp')
    assert 'acal_prog' in web and 'acal_done' in web and 'acal_error' in web
    for tok in ['phase', 'air', 'noise', 'confidence', 'validFrames', 'totalFrames',
                'nominal', 'stability', 'snr']:
        assert tok in web, tok
    ui = read('Servo_flute_ESP32/web_content.h')
    for tok in ['validFrames', 'acalNoise', "d.t==='acal_error'", 'r.nominal']:
        assert tok in ui, tok


def test_autocal_actuator_ownership_and_locks():
    # §8/§9/§10/§12: single-owner calibration, concurrent-command blocking,
    # config lock and monitor restore.
    web = read('Servo_flute_ESP32/WebConfigurator.cpp')
    hdr = read('Servo_flute_ESP32/WebConfigurator.h')
    assert '_autoCalOwnerClientId' in hdr and '_autoCalOwnerClientId' in web
    assert '_micMonitorBeforeCalibration' in hdr and '_micMonitorBeforeCalibration' in web
    assert 'cancelActiveActuatorSession' in web
    assert 'actuatorCommandBlockedDuringCalibration' in web
    assert 'calibration_busy' in web            # second start refused
    assert 'not_calibration_owner' in web        # non-owner stop/apply refused
    assert 'calibration_active' in web           # blocked commands + 409 config lock
    assert '409' in web                          # config POST lock status
    # Panic cancels the calibration before all-sound-off.
    panic = web.split('strcmp(type, "panic")', 1)[1].split('else if', 1)[0]
    assert 'cancelActiveActuatorSession' in panic
    # Owner-only disconnect stops the session.
    disc = web.split('WS_EVT_DISCONNECT', 1)[1].split('WS_EVT_DATA', 1)[0]
    assert '_autoCalOwnerClientId' in disc


def test_autocal_frame_freshness_contract():
    # §3: IAudioSource exposes frame freshness; AudioAnalyzer advances it.
    iface = read('Servo_flute_ESP32/IAudioSource.h')
    assert 'getFrameSequence' in iface and 'getFrameTimestamp' in iface
    aa = read('Servo_flute_ESP32/AudioAnalyzer.cpp')
    assert '_frameSeq++' in aa
    assert 'MIC_FRAME_STALE_MS' in aa
    acc = read('Servo_flute_ESP32/AutoCalibrator.cpp')
    assert 'getFrameSequence()' in acc
    assert 'ACAL_FAIL_AUDIO_STALE' in acc


def test_audio_dual_i2s_and_pitch_extraction():
    # §1/§14/§15: dual I2S (IDF 4.x legacy + 5.x std), extracted YIN core, mic status.
    aa = read('Servo_flute_ESP32/AudioAnalyzer.cpp')
    assert 'ESP_IDF_VERSION_VAL(5, 0, 0)' in read('Servo_flute_ESP32/AudioAnalyzer.h')
    assert 'i2s_driver_install' in aa       # legacy IDF 4.x path
    assert 'i2s_channel_read' in aa         # std IDF 5.x path
    assert 'resetMicrophone' in aa
    assert 'classifyRaw' in aa
    from pathlib import Path
    assert Path(ROOT / 'Servo_flute_ESP32/PitchDetector.cpp').exists()
    assert 'lib_ignore = native_stubs' in read('platformio.ini')
    web = read('Servo_flute_ESP32/WebConfigurator.cpp')
    assert 'mic_status' in web and 'mic_reset' in web


def test_airflow_uses_nominal_two_segment():
    # §2: playback pivots on the calibrated nominal.
    air = read('Servo_flute_ESP32/AirflowController.cpp')
    assert 'AIRFLOW_SOURCE_PIVOT' in air
    assert 'nominalAngle' in air


def test_autocal_global_timeout_and_injection_layer():
    # Firmware-side global timeout + a hardware-free audio injection interface.
    st = read('Servo_flute_ESP32/settings.h')
    assert 'AUTOCAL_GLOBAL_TIMEOUT_MS' in st
    acc = read('Servo_flute_ESP32/AutoCalibrator.cpp')
    assert 'AUTOCAL_GLOBAL_TIMEOUT_MS' in acc
    assert 'delay(' not in acc                    # non-blocking state machine
    assert Path(ROOT / 'Servo_flute_ESP32/IAudioSource.h').exists()
    ah = read('Servo_flute_ESP32/AudioAnalyzer.h')
    assert 'public IAudioSource' in ah


def test_autocal_air_supply_abstraction():
    # §7: the calibrator drives every air mode through ICalibrationAirSupply and
    # gates on readiness before measuring, without duplicating controller logic.
    assert Path(ROOT / 'Servo_flute_ESP32/ICalibrationAirSupply.h').exists()
    iface = read('Servo_flute_ESP32/ICalibrationAirSupply.h')
    for m in ('prepare()', 'isReady()', 'setDemandPercent(', 'stopSafe()', 'getError()'):
        assert m in iface, m
    # Concrete impl delegates to the same controllers used during play.
    impl = read('Servo_flute_ESP32/CalibrationAirSupply.cpp')
    assert '_fan.setSpeed' in impl and '_pressure.setTargetPercent' in impl
    for mode in ('AIR_MODE_FAN_SERVO', 'AIR_MODE_PUMP_VALVE', 'AIR_MODE_PUMP_RESERVOIR'):
        assert mode in impl, mode
    # Calibrator prepares the supply, gates on readiness, and fails with a specific
    # reason when it never comes up.
    acc = read('Servo_flute_ESP32/AutoCalibrator.cpp')
    assert '_airSupply.prepare()' in acc
    assert '_airSupply.isReady()' in acc
    assert '_airSupply.stopSafe()' in acc
    assert 'ACAL_FAIL_AIR_SUPPLY' in acc
    assert 'AUTOCAL_AIR_READY_TIMEOUT_MS' in acc
    # Wired through InstrumentManager (owner of the real controllers).
    im = read('Servo_flute_ESP32/InstrumentManager.h')
    assert 'getCalibrationAirSupply()' in im
    wc = read('Servo_flute_ESP32/WebConfigurator.cpp')
    assert 'getCalibrationAirSupply()' in wc


def test_autocal_review_fixes():
    # Wiring that lives in the non-host-compilable web/instrument layer; the
    # behaviour of the underlying calibrator/air-supply/power logic is covered by
    # the native behavioural tests.
    wc = read('Servo_flute_ESP32/WebConfigurator.cpp')
    # D1: rf_done is broadcast once and the range result is kept applicable (the
    # calibrator is NOT stopped when the range finder completes).
    assert '_rfDoneSent' in wc
    rf_block = wc.split('Check range finder completion', 1)[1].split('Check airflow calibration', 1)[0]
    assert '_autoCal->stop()' not in rf_block
    # D4: overall ok reflects applied AND saved (never a false partial success).
    assert 'ap.applied && ap.saved' in wc
    assert 'storage_failed' in wc
    # D6: pump_stop / fan_stop are blocked during a calibration.
    assert '"pump_stop", "fan_stop"' in wc
    # D7: reset and factory reset are guarded by the calibration lock.
    for fn in ['handleApiConfigReset', 'handleApiFactoryReset']:
        body = wc.split(f'void WebConfigurator::{fn}', 1)[1].split('\nvoid ', 1)[0]
        assert 'rejectIfCalibrationActive' in body, fn
    # D11: user strings serialized through ArduinoJson (jsonStr) in the config GET.
    assert 'json += ",\\"device\\":" + jsonStr(cfg.deviceName)' in wc
    # D2: calibration holds servo power via the actuator session.
    assert 'setActuatorSessionActive' in wc
    im = read('Servo_flute_ESP32/InstrumentManager.cpp')
    assert '_actuatorSessionActive' in im and 'ensureServosPowered()' in im
    # D3: dedicated range-finder failure path (never finalizeNote/advanceNote).
    ac = read('Servo_flute_ESP32/AutoCalibrator.cpp')
    assert 'finalizeRangeFinderFailure' in ac and 'failCurrentNote' in ac
    # D8: reservoir mode is strict about the sensor.
    cas = read('Servo_flute_ESP32/CalibrationAirSupply.cpp')
    assert 'CAL_AIR_SENSOR_FAULT' in cas and 'isSensorDetected()' in cas
    # D13: textual failure-reason names.
    assert 'failureReasonName' in ac


def test_review2_frontend_backend_contracts():
    # #19: server auto-cancels a pending range-finder result after a review window.
    wc = read('Servo_flute_ESP32/WebConfigurator.cpp')
    assert 'AUTOCAL_RF_REVIEW_TIMEOUT_MS' in wc and 'rf_expired' in wc
    web = read('Servo_flute_ESP32/web_content.h')
    # #19: dismissing a range result actually cancels it on the server.
    assert "function dismissRangeResult(){wsSend({t:'auto_cal',mode:'stop'})" in web
    # #18: acal_done / rf_applied honour the persisted outcome instead of always
    # reporting success.
    assert 'if(d.ok){' in web and 'acalErrText' in web
    # #14 / #2 backend markers already covered by the native tests; #20 angle:
    ac_cpp = read('Servo_flute_ESP32/AirflowController.cpp')
    assert '_lastSentAirflowAngle = angle' in ac_cpp
    im = read('Servo_flute_ESP32/InstrumentManager.cpp')
    assert 'if (_actuatorSessionActive) return' in im


def test_watchdog_has_idf_compatibility_layer():
    ino = read('Servo_flute_ESP32/Servo_flute_ESP32.ino')
    assert '#include <esp_idf_version.h>' in ino
    assert 'bool initializeWatchdog()' in ino
    assert 'ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)' in ino
    assert 'esp_task_wdt_init(&wdt_config)' in ino
    assert 'esp_task_wdt_init(WATCHDOG_TIMEOUT_MS / 1000, true)' in ino


def test_boot_oe_stays_disabled_until_safe_outputs():
    src = read('Servo_flute_ESP32/InstrumentManager.cpp')
    fn = src.split('bool InstrumentManager::beginSafe()', 1)[1].split('void InstrumentManager::initializeSafeOutputs()', 1)[0]
    assert 'digitalWrite(PIN_SERVOS_OFF, HIGH)' in fn
    assert fn.index('_pwm0.begin()') < fn.index('initializeSafeOutputs()') < fn.index('powerOnServos()')
    assert 'powerOnServos();' not in src.split('bool InstrumentManager::beginSafe()', 1)[0]


def test_invalid_boot_config_prevents_actuators_and_reports_diagnostics():
    ino = read('Servo_flute_ESP32/Servo_flute_ESP32.ino')
    web = read('Servo_flute_ESP32/WebConfigurator.cpp')
    assert 'ConfigStorage::loadWithStatus()' in ino
    assert 'CONFIG_INVALID_FALLBACK' in ino
    assert 'instrument = nullptr' in ino
    assert 'config_load_status' in web and 'boot_config' in web


def test_midi_bounds_checked_before_seen_index():
    src = read('Servo_flute_ESP32/ConfigValidator.cpp')
    midi = src.split('bool midiSeen[128]', 1)[1].split('for (uint8_t i = 0; i < MAX_PUMPS', 1)[0]
    assert 'if (midiNote > 127)' in midi
    assert midi.index('if (midiNote > 127)') < midi.index('midiSeen[midiNote]')


def test_solenoid_modes_and_gpio_filter_helpers():
    src = read('Servo_flute_ESP32/ConfigValidator.cpp')
    air = read('Servo_flute_ESP32/AirflowController.cpp')
    assert 'bool modeUsesPhysicalValve(uint8_t airMode)' in src
    for mode in ['AIR_MODE_SOLENOID_SERVO', 'AIR_MODE_PUMP_VALVE', 'AIR_MODE_PUMP_RESERVOIR']:
        assert mode in src
    assert 'configurationUsesSolenoidValve(cfg)' in air
    assert 'pinMode(cfg.solenoidPin, OUTPUT)' in air
    assert 'closeSolenoid();  // apply closed level immediately' in air
    assert 'if (configurationUsesSolenoidValve(config)) addGpio(config.solenoidPin, true, false);' in src

def test_web_json_uses_arduinojson_for_unsafe_strings_and_special_cases_documented():
    src = read('Servo_flute_ESP32/WebConfigurator.cpp')
    for fn_name in ['handleApiStatus', 'handleMidiList', 'handleApiDiagnostics']:
        fn = src.split(f'void WebConfigurator::{fn_name}', 1)[1].split('\nvoid ', 1)[0]
        assert 'JsonDocument doc' in fn
        assert 'serializeJson(doc' in fn
    assert 'player["file"] = _player->getFileName()' in src
    assert 'entry["name"] = fname' in src
    docs = read('docs/API_WEB.md')
    assert 'Flute "A"' in docs
    assert 'atelier\\wifi' in docs
    assert 'étude "test".mid' in docs


def test_websocket_has_separate_midi_percent_and_servo_parsers():
    src = read('Servo_flute_ESP32/WebConfigurator.cpp')
    helpers = read('Servo_flute_ESP32/WebValueParsers.h')
    assert 'uint8_t getMidi7Bit' in helpers
    assert 'uint8_t getPercent' in helpers
    assert 'uint16_t getServoAngle' in helpers
    assert 'clampMidi7Bit(value.as<int>())' in helpers
    assert 'clampPercent(value.as<int>())' in helpers
    assert 'clampServoAngle(value.as<int>())' in helpers
    assert '_instrument->handleControlChange((uint8_t)getMidi7Bit(doc, "c", 0), getMidi7Bit(doc, "v", 0))' in src
    assert 'handleControlChange((uint8_t)constrain(doc["c"] | 0, 0, 127), getPercent(doc, "v", 0))' not in src


def test_post_body_limits_reset_and_avoid_malloc_fragments():
    src = read('Servo_flute_ESP32/WebConfigurator.cpp')
    hdr = read('Servo_flute_ESP32/WebConfigurator.h')
    # #5: POST bodies are per-request (request->_tempObject), not a shared member,
    # so concurrent requests can no longer clobber or mix each other's bodies.
    assert '_configBody' not in src and '_configBody' not in hdr
    assert 'request->_tempObject' in src
    assert 'struct WebReqBody' in src
    assert 'tooLarge' in src
    assert 'Body too large' in src
    assert 'CONFIG_MAX_POST_BYTES' in src
    assert 'concat((const char*)data, len)' in src
    assert 'malloc(len + 1)' not in src
    # wifi/connect sends its response from a single handler, not the body callback.
    assert 'void WebConfigurator::handleApiWifiConnect' in src


def test_manual_test_session_server_side():
    # #4: manual actuator tests are bounded server-side (owner + timeout), and a
    # disconnect only safes the hardware for the test owner (not any WS client).
    wc = read('Servo_flute_ESP32/WebConfigurator.cpp')
    st = read('Servo_flute_ESP32/settings.h')
    assert 'TEST_SESSION_MAX_MS' in st and 'TEST_SESSION_MAX_MS' in wc
    assert 'beginTestSession' in wc and 'endTestSession' in wc
    assert 'isManualTestCommand' in wc
    assert 'client->id() == _testOwnerClientId' in wc
    # the old unconditional allSoundOff() on any disconnect is gone.
    assert 'if (_testActive && client->id() == _testOwnerClientId)' in wc


def test_audit_p0_boot_and_rest_safety():
    im = read('Servo_flute_ESP32/InstrumentManager.cpp')
    imh = read('Servo_flute_ESP32/InstrumentManager.h')
    # P0.1: OE is not enabled until every channel is programmed (init window).
    assert '_initializingHardware' in im and '_initializingHardware' in imh
    assert '_initializingHardware = true' in im and '_initializingHardware = false' in im
    # P0.2: actuator paths are inert unless the hardware initialised successfully.
    assert 'isHardwareReady' in imh
    assert im.count('_hardwareInitStatus != HW_INIT_OK') >= 4   # setPWM/update/noteOn/noteOff/CC/power
    af = read('Servo_flute_ESP32/AirflowController.cpp')
    # P0.4: return-to-rest cancels attack/vibrato and the attack write is sound-gated.
    assert '_attackActive = false' in af and '_vibratoActive = false' in af
    assert '_solenoidOpen && (_attackActive' in af
    # P0.5: CC2 timeout falls back to velocity on a held note from update().
    assert '_cc2TimedOut' in af and 'recomputeActiveNote()' in af


def test_audit_p0_controlled_restart_on_reboot_required_config():
    web = read('Servo_flute_ESP32/WebConfigurator.cpp')
    webh = read('Servo_flute_ESP32/WebConfigurator.h')
    cfg = read('Servo_flute_ESP32/ConfigStorage.cpp')
    cfgh = read('Servo_flute_ESP32/ConfigStorage.h')
    st = read('Servo_flute_ESP32/settings.h')
    # P0.3/P0.6: a controlled reboot is scheduled and executed from update().
    assert '_pendingRestartTime' in web and '_pendingRestartTime' in webh
    assert 'void scheduleControlledRestart()' in webh
    assert 'scheduleControlledRestart()' in web
    assert 'restartPending()' in web
    assert 'ESP.restart()' in web
    assert 'CONFIG_RESTART_DELAY_MS' in st and 'CONFIG_RESTART_DELAY_MS' in web
    # Config-mutating routes refuse while a reboot is pending so they cannot
    # overwrite the persisted pending config before it applies.
    assert web.count('restart_pending') >= 3   # finalize + reset + factory
    # P0.3: reboot is scheduled for the reboot-required finalize path only when saved.
    assert 'if (saved) scheduleControlledRestart();' in web
    # P0.6: reset / factory-reset report success from the storage layer and reboot.
    assert 'bool ConfigStorage::resetToDefaults()' in cfg
    assert 'bool ConfigStorage::factoryReset()' in cfg
    assert 'static bool resetToDefaults();' in cfgh
    assert 'static bool factoryReset();' in cfgh
    assert web.count('if (ok) scheduleControlledRestart();') >= 2


def test_audit_p1_actuator_ownership_and_playback_isolation():
    web = read('Servo_flute_ESP32/WebConfigurator.cpp')
    webh = read('Servo_flute_ESP32/WebConfigurator.h')
    wm = read('Servo_flute_ESP32/WirelessManager.cpp')
    # P1 #8: the physical double-press does not fight an owned actuator session.
    assert 'isActuatorSessionActive()' in wm
    assert 'openAllFingers' in wm
    # The guard precedes the direct actuator drive.
    assert wm.index('isActuatorSessionActive()') < wm.index('getFingerCtrl().openAllFingers()')
    # P1 #9: a running MIDI playback is paused before a calibration takes the actuators.
    # Anchor on the calibration-start comment so this is not satisfied by the unrelated
    # WS "pause" command handler.
    marker = 'drive notes into the actuators the calibration is'
    assert marker in web
    assert web.index(marker) < web.index('_autoCal->start(')
    # P1 #10: manual-test ownership cannot be hijacked by a competing client.
    assert 'bool beginTestSession(uint32_t clientId)' in webh
    assert 'bool WebConfigurator::beginTestSession' in web
    assert 'test_busy' in web
    assert '_testOwnerClientId != 0 && clientId != _testOwnerClientId' in web


def test_audit_p1_air_source_tied_to_sequencer_transitions():
    im = read('Servo_flute_ESP32/InstrumentManager.cpp')
    nsh = read('Servo_flute_ESP32/NoteSequencer.h')
    # P1 §6/§7: the direct pump / fan demand is driven from the sequencer's real note
    # transitions, not from raw incoming MIDI events.
    assert 'updateAirSourceFromSequencer' in im
    assert 'getCurrentVelocity' in nsh
    assert 'STATE_POSITIONING && _prevSequencerState != STATE_POSITIONING' in im
    assert 'STATE_IDLE && _prevSequencerState != STATE_IDLE' in im
    # noteOn / noteOff must no longer set the pump/fan demand themselves.
    note_on = im.split('InstrumentManager::noteOn')[1].split('InstrumentManager::noteOff')[0]
    note_off = im.split('InstrumentManager::noteOff')[1].split('InstrumentManager::isNotePlayable')[0]
    assert '_pressureCtrl.setTargetPercent' not in note_on
    assert '_fanCtrl.setSpeed' not in note_on
    assert '_pressureCtrl.setTargetPercent' not in note_off


def test_audit_p1_test_note_and_pump_commands():
    web = read('Servo_flute_ESP32/WebConfigurator.cpp')
    pc = read('Servo_flute_ESP32/PressureController.cpp')
    pch = read('Servo_flute_ESP32/PressureController.h')
    # §11: test_note plays a real timed note (via the sequencer) and schedules a stop.
    test_note = web.split('strcmp(type, "test_note") == 0')[1].split('else if')[0]
    assert '_instrument->noteOn(' in test_note
    assert '_testNoteOffTime' in web and 'TEST_NOTE_DURATION_MS' in web
    assert '_instrument->noteOff(_testNoteMidi)' in web
    # §12: pump_enable is handled (no longer Unknown message type).
    assert '"pump_enable"' in web and 'setEnabled(' in web
    # §12: pump_target / pump_stop honour a per-pump index.
    assert 'testSinglePump(' in web and 'stopSinglePumpTest(' in web
    assert 'doc["pump"]' in web
    # PressureController exposes the mute + single-pump primitives.
    assert 'void setEnabled(bool enabled)' in pch
    assert 'void testSinglePump(uint8_t index, uint8_t percent)' in pch
    assert 'void PressureController::setEnabled' in pc
    assert 'void PressureController::testSinglePump' in pc
    # A single-pump test must override normal control in update().
    assert '_testPumpIndex >= 0' in pc


def test_audit_p1_transactional_config_save():
    cfg = read('Servo_flute_ESP32/ConfigStorage.cpp')
    web = read('Servo_flute_ESP32/WebConfigurator.cpp')
    # §15: save() writes a temp file, verifies it re-parses, then atomically renames.
    assert 'CONFIG_FILE_PATH ".tmp"' in cfg
    assert 'LittleFS.rename(' in cfg
    assert 'deserializeJson(check' in cfg
    # The real config file is not truncated directly on the write path any more.
    save_body = cfg.split('bool ConfigStorage::save()')[1].split('ConfigStorage::')[0]
    assert 'LittleFS.open(CONFIG_FILE_PATH, "w")' not in save_body
    assert 'LittleFS.open(tmpPath, "w")' in save_body
    # loadWithStatus recovers an interrupted save (temp promoted / discarded on boot).
    load_body = cfg.split('ConfigStorage::loadWithStatus()')[1].split('ConfigStorage::')[0]
    assert 'LittleFS.exists(tmpPath)' in load_body
    # §14: a failed non-restart save rolls back the live config + controllers.
    assert 'applyRuntimeConfig(failedConfig, previousConfig)' in web


def test_audit_p1_gpio_validation_completeness():
    val = read('Servo_flute_ESP32/ConfigValidator.cpp')
    st = read('Servo_flute_ESP32/settings.h')
    # §18: flash pins, I2S mic pins and the full input-only 34-39 range are covered.
    assert 'isFlashGpio' in val and 'isI2sMicGpio' in val
    assert 'pin >= 34 && pin <= 39' in val
    assert 'pin >= 6 && pin <= 11' in val
    # The serial-MIDI RX pin is now added to the validated used-pin set.
    assert 'config.serialMidiEnabled' in val and 'config.serialMidiRxPin' in val
    # Endstop pins needing INPUT_PULLUP are rejected on the pull-up-less 34-39 pins.
    assert 'needs a pull-up' in val
    # The default endstop pin is no longer the pull-up-less GPIO34.
    assert '#define DEFAULT_ENDSTOP_PIN         34' not in st
    assert 'DEFAULT_ENDSTOP_PIN' in st


def test_audit_p1_nonblocking_tof():
    pc = read('Servo_flute_ESP32/PressureController.cpp')
    st = read('Servo_flute_ESP32/settings.h')
    # §19: the blocking single-shot poll loop (delay(1) x50) is gone.
    assert 'serviceTofMeasurement' in pc
    assert 'delay(1)' not in pc
    tof = pc.split('bool PressureController::serviceTofMeasurement')[1].split('\n}\n')[0]
    assert 'for (int i = 0; i < 50' not in tof
    # §20: a timeout marks the measurement invalid + counts errors instead of reading
    # a bogus range, and staleness cuts the pump.
    assert '_measurementValid' in pc and '_tofErrorCount' in pc
    assert 'TOF_RANGE_TIMEOUT_MS' in st and 'TOF_STALE_MS' in st and 'TOF_MAX_CONSEC_ERRORS' in st
    assert '_measurementValid && (now - _lastValidReadTime) >= TOF_STALE_MS' in pc


def test_diagnostics_status_vocabulary_and_passive_active_split():
    src = read('Servo_flute_ESP32/WebConfigurator.cpp')
    api = read('docs/API_WEB.md')
    for status in ['ok', 'warning', 'error', 'not_tested', 'not_applicable']:
        assert status in src or status in api
    assert '"/api/diagnostics"' in src
    assert '"/api/diagnostics/run"' in src
    assert 'passive' in api.lower() and 'active' in api.lower()


def test_gmb_protocol_lives_in_one_place_not_per_transport():
    """The wire protocol is implemented once; a transport is only a port."""
    core = read('Servo_flute_ESP32/gmb/GmbSysEx.cpp')
    # Frame construction happens in the codec, nowhere else.
    assert '0x7D' in read('Servo_flute_ESP32/gmb/GmbSysEx.h')
    for transport in ('Servo_flute_ESP32/BleMidiHandler.cpp',
                      'Servo_flute_ESP32/WifiMidiHandler.cpp',
                      'Servo_flute_ESP32/SerialMidiHandler.cpp',
                      'Servo_flute_ESP32/WebConfigurator.cpp',
                      'Servo_flute_ESP32/Servo_flute_ESP32.ino'):
        src = read(transport)
        for token in ('0x7D', '0xF0, 0x7D', 'encodeHandshake', 'encodeDescriptorChunk',
                      'encodeChangeNotification', 'parseRequest'):
            assert token not in src, f'{transport} duplicates GMB protocol logic: {token}'
    assert 'encodeHandshake' in core
    assert 'encodeDescriptorChunk' in core
    assert 'encodeChangeNotification' in core


def test_gmb_bidirectional_transports_are_registered_and_din_is_not():
    """DIN MIDI is RX-only on this board: it cannot answer and is not a GMB port."""
    wm = read('Servo_flute_ESP32/WirelessManager.cpp')
    assert 'registerPort(&_bleMidi)' in wm
    assert 'registerPort(&_wifiMidi)' in wm
    assert 'registerPort(&_serialMidi)' not in wm
    # ... and it keeps working normally for Note / CC.
    serial = read('Servo_flute_ESP32/SerialMidiHandler.h')
    assert 'IGmbMidiPort' not in serial
    for handler in ('Servo_flute_ESP32/BleMidiHandler.h', 'Servo_flute_ESP32/WifiMidiHandler.h'):
        assert 'public gmb::IGmbMidiPort' in read(handler)
        assert 'canSendSysEx' in read(handler)


def test_gmb_replies_are_built_off_the_realtime_callback():
    """SysEx callbacks only stage; the response is built from the main loop."""
    bridge = read('Servo_flute_ESP32/gmb/GmbMidiBridge.cpp')
    stage = bridge.split('void GmbMidiBridge::onSysEx', 1)[1].split('void GmbMidiBridge::service', 1)[0]
    assert 'handleMessage' not in stage, 'a reply must not be built in the MIDI callback'
    assert 'memcpy' in stage
    assert 'handleMessage' in bridge.split('void GmbMidiBridge::service', 1)[1]
    for handler in ('Servo_flute_ESP32/BleMidiHandler.cpp', 'Servo_flute_ESP32/WifiMidiHandler.cpp'):
        cb = read(handler).split('onSystemExclusive', 2)[2].split('\n}', 1)[0]
        assert 'bridge().onSysEx' in cb
        assert 'sendSysEx' not in cb
    assert 'gmb::runtime::bridge().service(millis())' in read('Servo_flute_ESP32/WirelessManager.cpp')


def test_gmb_descriptor_is_cached_not_rebuilt_per_request():
    svc = read('Servo_flute_ESP32/gmb/GmbSysExService.cpp')
    handler = svc.split('GmbSysExService::handleMessage', 1)[1]
    assert 'GmbDescriptor::toJson' not in handler, 'no JSON render on the request path'
    assert 'GmbDescriptor::toJson' in svc.split('GmbSysExService::setSnapshot', 1)[1].split('}', 1)[0]
    # A transfer in flight is pinned to the document it started on.
    assert '_serving' in svc
    # Abusive traffic is bounded.
    assert 'allow(nowMs)' in handler


def test_gmb_notification_only_after_a_committed_active_configuration():
    web = read('Servo_flute_ESP32/WebConfigurator.cpp')
    fn = web.split('void WebConfigurator::handleApiConfigFinalize', 1)[1]
    # Never for an intermediate draft: validated, saved, and not pending a reboot.
    assert 'if (saved && !restartRequired) {\n      gmb::runtime::onConfigurationActivated();' in fn
    runtime = read('Servo_flute_ESP32/gmb/GmbRuntime.cpp')
    activated = runtime.split('void onConfigurationActivated', 1)[1]
    # The revision only moves when onConfigurationActivated() says the announced
    # capabilities changed; otherwise the function returns without notifying.
    assert 'if (!g_revision.onConfigurationActivated(signature, false)) {' in activated
    # After the increment: persist -> rebuild the descriptor -> emit block 0x11.
    tail = activated.split('persist(g_revision.revision()', 1)[1]
    assert tail.index('setSnapshot(') < tail.index('notifyCapabilitiesChanged(')
    assert 'notifyCapabilitiesChanged' not in activated.split('persist(g_revision.revision()', 1)[0]


def test_gmb_revision_persists_outside_the_configuration_file():
    runtime = read('Servo_flute_ESP32/gmb/GmbRuntime.cpp')
    assert 'Preferences' in runtime
    assert 'kNvsNamespace' in runtime
    # A configuration save must not rewrite the counter, and vice versa.
    storage = read('Servo_flute_ESP32/ConfigStorage.cpp')
    assert 'capabilitiesRevision' not in storage
    assert 'gmb' not in storage.split('bool ConfigStorage::save()', 1)[1]
    # A boot never increments on its own.
    revision = read('Servo_flute_ESP32/gmb/GmbRevision.cpp')
    begin = revision.split('bool RevisionTracker::begin', 1)[1].split('bool RevisionTracker::onConfigurationActivated', 1)[0]
    assert 'if (storedSignature == current.all) {' in begin
    assert 'return false;' in begin


def test_gmb_http_and_sysex_serve_the_same_document():
    web = read('Servo_flute_ESP32/WebConfigurator.cpp')
    assert '"/gmb/descriptor.json"' in web
    assert 'gmb::runtime::descriptorJson()' in web
    runtime = read('Servo_flute_ESP32/gmb/GmbRuntime.h')
    assert 'descriptorJson' in runtime
    # Flag bit 0 follows the web server, which only runs in Wi-Fi mode.
    assert 'gmb::runtime::setHttpDescriptorAvailable(true);' in web
    assert 'setHttpDescriptorAvailable' in read('Servo_flute_ESP32/gmb/GmbSysExService.h')


def test_gmb_capabilities_come_from_the_active_configuration_only():
    caps = read('Servo_flute_ESP32/gmb/Capabilities.cpp')
    # Every announced capability is read off RuntimeConfig, not hard-coded.
    for field in ('config.numNotes', 'config.numFingers', 'config.midiChannel',
                  'config.embouchure', 'config.servoToSolenoidDelayMs',
                  'config.minNoteDurationMs', 'config.cc2Enabled',
                  'config.airVelocityResponse', 'config.angleServoEnabled',
                  'config.vibratoMaxAmplitudeDeg'):
        assert field in caps, f'{field} must feed the announced capabilities'
    # No second configuration model.
    assert 'struct RuntimeConfig' not in caps
    assert '#include "../ConfigStorage.h"' in caps
    # The firmware version has a single source.
    assert 'FIRMWARE_VERSION_MAJOR' in caps
    assert '#define FIRMWARE_VERSION_MAJOR' in read('Servo_flute_ESP32/settings.h')


def test_gmb_class_constants_have_an_out_of_class_definition():
    """Every `static constexpr` member of a gmb class is defined out of class.

    Before C++17 a `static constexpr` data member that is odr-used - passed to
    `std::vector::push_back(const uint8_t&)`, bound to a reference, taken the
    address of - needs a namespace-scope definition. The ESP32 Arduino toolchain
    builds this firmware with a pre-C++17 dialect, so omitting one does not fail
    the host tests (they run at C++17) but breaks the firmware LINK with
    "undefined reference to gmb::GmbSysEx::kStart".

    Guarding it here catches the mistake in seconds instead of at the end of an
    ESP32 build.
    """
    import re

    member = re.compile(
        r'^\s*static\s+constexpr\s+([A-Za-z_][\w:]*)\s+(k[A-Za-z0-9_]*)\s*=', re.M)
    checked = 0
    for header in sorted((ROOT / 'Servo_flute_ESP32/gmb').glob('*.h')):
        source = header.with_suffix('.cpp')
        if not source.exists():
            continue
        text = read(f'Servo_flute_ESP32/gmb/{header.name}')
        impl = read(f'Servo_flute_ESP32/gmb/{source.name}')
        cls = None
        for line in text.splitlines():
            found = re.match(r'\s*class\s+([A-Za-z_]\w*)\s*\{', line)
            if found:
                cls = found.group(1)
            hit = member.match(line)
            if not hit or cls is None:
                continue
            kind, name = hit.group(1), hit.group(2)
            expected = f'constexpr {kind} {cls}::{name};'
            assert expected in impl, (
                f'{source.name} must define {cls}::{name} out of class '
                f'("{expected}"), or the ESP32 link fails when it is odr-used')
            checked += 1
    assert checked >= 7, 'the GmbSysEx constants must be covered by this check'
    sysex = read('Servo_flute_ESP32/gmb/GmbSysEx.cpp')
    assert '#if __cplusplus < 201703L' in sysex, (
        'the out-of-class definitions are deprecated from C++17 on and must be guarded')


def test_gmb_excite_latency_is_not_derived_from_an_actuator_delay():
    """`timing.excite.latency_ms` is acoustic, and nothing measures it yet.

    `solenoidActivationTimeMs` is the solenoid's full-power drive window before
    the PWM drops to its holding level - an electrical parameter of the coil, not
    the moment the air column speaks. Announcing it as the excitation latency
    would make General-Midi-Boop schedule this flute against a figure it does not
    honour, so the field stays absent (GMB: absent = unknown) until a real
    measurement exists.
    """
    caps = read('Servo_flute_ESP32/gmb/Capabilities.cpp')
    assert 'config.solenoidActivationTimeMs' not in caps
    assert 'uint16_t measuredExciteLatencyMs(const RuntimeConfig& config)' in caps
    assert 'uint16_t measuredExciteLatencyMs(const RuntimeConfig& config);' in \
        read('Servo_flute_ESP32/gmb/Capabilities.h')
    # The single seam a measured value lands in, and the only thing that can turn
    # the announcement on.
    body = caps.split('CapabilitySnapshot buildSnapshot', 1)[1]
    assert 'const uint16_t exciteMs = measuredExciteLatencyMs(config);' in body
    assert 'if (exciteMs > 0) {' in body
    # The rest of the two-phase timing model is still announced.
    for known in ('hasPrepare', 'hasMinNote', 'hasRearticulation'):
        assert known in body
    # ...and the serialiser still omits an unknown rather than emitting a zero.
    descriptor = read('Servo_flute_ESP32/gmb/GmbDescriptor.cpp')
    assert 'if (t.hasExciteLatency) {' in descriptor


def test_gmb_transfer_pin_is_chosen_once_per_transfer():
    """A retry of segment 0 must not re-pin the transfer onto a newer document.

    The regression: `if (!_serving || index == 0 || stale) _serving = _descriptor;`
    re-selected the snapshot on every segment-0 request, so a controller that
    retried segment 0 after a configuration save reassembled segment 0 of one
    revision with segment 1 of the next.
    """
    svc = read('Servo_flute_ESP32/gmb/GmbSysExService.cpp')
    serve = svc.split('GmbSysExService::serveDescriptorChunk', 1)[1].split('\n}', 1)[0]
    # The document is chosen on the single question "is a transfer in flight?",
    # never on which segment was asked for.
    assert 'const bool starting = !_serving;' in serve
    assert 'index ==' not in serve, 'the pin must not depend on the segment index'
    assert 'index == 0' not in svc
    # Released only when the document has been delivered in full, on the idle
    # timeout, or when a handshake contradicts it.
    assert 'expireStaleTransfer' in serve
    assert '_servingDelivered >= (uint32_t)total' in serve
    handler = svc.split('GmbSysExService::handleMessage', 1)[1]
    assert 'if (_serving && _serving != _descriptor) endTransfer();' in handler

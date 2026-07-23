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
    assert 'if (configurationUsesSolenoidValve(config)) gpios[gcount++] = config.solenoidPin;' in src

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
    assert '_configBodyTooLarge' in hdr and '_configBodyTooLarge' in src
    assert 'Body too large' in src
    assert 'CONFIG_MAX_POST_BYTES' in src
    assert 'concat((const char*)data, len)' in src
    assert 'malloc(len + 1)' not in src
    assert '_configBodyTooLarge = false' in src


def test_diagnostics_status_vocabulary_and_passive_active_split():
    src = read('Servo_flute_ESP32/WebConfigurator.cpp')
    api = read('docs/API_WEB.md')
    for status in ['ok', 'warning', 'error', 'not_tested', 'not_applicable']:
        assert status in src or status in api
    assert '"/api/diagnostics"' in src
    assert '"/api/diagnostics/run"' in src
    assert 'passive' in api.lower() and 'active' in api.lower()

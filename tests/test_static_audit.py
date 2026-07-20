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

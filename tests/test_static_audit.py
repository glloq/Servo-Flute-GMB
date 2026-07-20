from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]

def read(path):
    return (ROOT / path).read_text(encoding='utf-8')

def test_validation_entry_points_present():
    assert 'validateAndNormalizeConfig(RuntimeConfig& config' in read('Servo_flute_ESP32/ConfigStorage.h')
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

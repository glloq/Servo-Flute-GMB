from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]

def read(path):
    return (ROOT / path).read_text(encoding='utf-8')

def test_validation_entry_points_present():
    assert 'validateAndNormalizeConfig(RuntimeConfig& config' in read('Servo_flute_ESP32/ConfigStorage.h')
    assert 'validateAndNormalizeConfig(RuntimeConfig& config' in read('Servo_flute_ESP32/ConfigValidator.cpp')
    assert 'validateAndNormalizeConfig(cfg' in read('Servo_flute_ESP32/Servo_flute_ESP32.ino')
    # The web path now validates a CANDIDATE copy, never the active config, and
    # the commit lives in ConfigCommit.cpp (see the transactional-commit test).
    assert 'validateAndNormalizeConfig(candidate, &active)' in read('Servo_flute_ESP32/ConfigCommit.cpp')

def test_websocket_uses_arduinojson_not_indexof_type_parsing():
    src = read('Servo_flute_ESP32/WebConfigurator.cpp')
    fn = src.split('void WebConfigurator::processWsMessage', 1)[1].split('void WebConfigurator::broadcastStatus', 1)[0]
    assert 'deserializeJson(doc, data, len)' in fn
    assert 'indexOf("\\"t' not in fn
    assert 'len > 512' in fn

def test_legacy_angle_migration_and_no_valve_dir_save():
    src = read('Servo_flute_ESP32/ConfigStorage.cpp')
    assert 'doc["angle_ch"] = source.angleServoPcaChannel' in src
    save_fn = src.split('bool ConfigStorage::saveFrom(', 1)[1]
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
    assert 'n["anm"] = source.notes[i].airflowNominalPercent' in cs   # save
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
    # Panic cancels the calibration before safing the hardware. Both now run on the
    # loop task: the cancel is a deferred web op, the safe state a non-droppable
    # panic request (an AsyncTCP callback must never drive actuators itself).
    panic = web.split('strcmp(type, "panic")', 1)[1].split('else if', 1)[0]
    assert 'WEBOP_AUTOCAL_CANCEL' in panic
    assert 'requestPanic()' in panic
    assert panic.index('WEBOP_AUTOCAL_CANCEL') < panic.index('requestPanic()')
    assert 'allSoundOff()' not in panic
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
    # The CC is still parsed with the MIDI 7-bit helper, but it is now POSTED to the
    # command queue instead of being applied from the AsyncTCP callback.
    assert 'postCommand(ACMD_CONTROL_CHANGE, getMidi7Bit(doc, "c", 0), getMidi7Bit(doc, "v", 0))' in src
    assert 'handleControlChange((uint8_t)constrain(doc["c"] | 0, 0, 127), getPercent(doc, "v", 0))' not in src
    assert '_instrument->handleControlChange(' not in src


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
    # ESPAsyncWebServer frees _tempObject with free() in the request destructor, so
    # the per-request body must be a POD allocated with malloc(): a C++ object with a
    # String member would leak on every aborted request and be free()d after new.
    assert 'malloc(sizeof(WebReqBody) + capacity)' in src
    assert 'new WebReqBody' not in src
    assert 'free(b)' in src
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
    # P0.3: the reboot is scheduled from the commit result only when the new config
    # was actually persisted AND needs a hardware re-init.
    commit = web.split('case WEBOP_COMMIT_CONFIG', 1)[1].split('case WEBOP_RESET_CONFIG', 1)[0]
    assert 'if (res.restartRequired) {' in commit
    assert 'scheduleControlledRestart();' in commit
    assert commit.index('resp["saved"] = true;') < commit.index('scheduleControlledRestart();')
    # P0.6: reset / factory-reset report success from the storage layer and reboot.
    assert 'bool ConfigStorage::resetToDefaults()' in cfg
    assert 'bool ConfigStorage::factoryReset()' in cfg
    assert 'static bool resetToDefaults();' in cfgh
    assert 'static bool factoryReset();' in cfgh
    # Reset and factory reset both reboot from the same deferred-op branch.
    reset = web.split('case WEBOP_RESET_CONFIG', 1)[1].split('case WEBOP_RESTART', 1)[0]
    assert 'ConfigStorage::resetToDefaults()' in reset and 'ConfigStorage::factoryReset()' in reset
    assert 'if (ok) scheduleControlledRestart();' in reset


def test_audit_p1_actuator_ownership_and_playback_isolation():
    web = read('Servo_flute_ESP32/WebConfigurator.cpp')
    webh = read('Servo_flute_ESP32/WebConfigurator.h')
    wm = read('Servo_flute_ESP32/WirelessManager.cpp')
    # P1 #8: the physical double-press does not fight an owned actuator session.
    assert 'isActuatorSessionActive()' in wm
    # The physical double-press now goes through the command queue like every other
    # actuator order, so it also carries the central hardware_not_ready guard.
    assert 'postCommand(ACMD_OPEN_ALL_FINGERS)' in wm
    assert 'getFingerCtrl().openAllFingers()' not in wm
    # The guard precedes the command.
    assert wm.index('isActuatorSessionActive()') < wm.index('postCommand(ACMD_OPEN_ALL_FINGERS)')
    # P1 #9: a running MIDI playback is paused before a calibration takes the actuators.
    # Anchor on the calibration-start comment so this is not satisfied by the unrelated
    # WS "pause" command handler.
    marker = 'notes vers les actionneurs que la calibration'
    assert marker in web
    assert web.index(marker) < web.index('_autoCal->start(')
    start_branch = web.split('case WEBOP_AUTOCAL_START_AIR', 1)[1].split('case WEBOP_AUTOCAL_CANCEL', 1)[0]
    assert '_player->pause();' in start_branch
    assert start_branch.index('_player->pause();') < start_branch.index('_autoCal->start(')
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
    # A panic must STICK: allSoundOff() aligns _prevSequencerState so the forced
    # return to STATE_IDLE is not read as a normal note end on the next update
    # (which would put the pump back to its idle demand right after stopping it).
    aso = im.split('void InstrumentManager::allSoundOff()')[1].split('\n}\n')[0]
    assert '_prevSequencerState = STATE_IDLE;' in aso


def test_audit_p1_test_note_and_pump_commands():
    web = read('Servo_flute_ESP32/WebConfigurator.cpp')
    pc = read('Servo_flute_ESP32/PressureController.cpp')
    pch = read('Servo_flute_ESP32/PressureController.h')
    # §11: test_note plays a real timed note (via the sequencer) and schedules a stop.
    test_note = web.split('strcmp(type, "test_note") == 0')[1].split('else if')[0]
    assert 'postCommand(ACMD_NOTE_ON, note, _webVelocity)' in test_note
    assert '_testNoteOffTime' in web and 'TEST_NOTE_DURATION_MS' in web
    assert 'postCommand(ACMD_NOTE_OFF, _testNoteMidi)' in web
    # §12: pump_enable is handled (no longer Unknown message type).
    assert '"pump_enable"' in web and 'ACMD_PUMP_ENABLE' in web
    # §12: pump_target / pump_stop honour a per-pump index.
    assert 'ACMD_PUMP_SINGLE_TEST' in web and 'ACMD_PUMP_STOP_SINGLE' in web
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
    save_body = cfg.split('bool ConfigStorage::saveFrom(')[1].split('bool ConfigStorage::save()')[0]
    assert 'LittleFS.open(CONFIG_FILE_PATH, "w")' not in save_body
    assert 'LittleFS.open(tmpPath, "w")' in save_body
    # save() is only a thin wrapper over saveFrom(cfg): the transactional commit
    # persists the CANDIDATE before it is ever made active.
    assert 'bool ConfigStorage::save() {\n  return saveFrom(cfg);\n}' in cfg
    # loadWithStatus recovers an interrupted save (temp promoted / discarded on boot).
    load_body = cfg.split('ConfigStorage::loadWithStatus()')[1].split('ConfigStorage::')[0]
    assert 'LittleFS.exists(tmpPath)' in load_body
    # §14: the commit is transactional - a failed save never activates anything, so
    # there is no applied-but-unsaved state to roll back afterwards.
    commit = read('Servo_flute_ESP32/ConfigCommit.cpp')
    assert 'out.saved = save ? save(candidate) : false;' in commit
    assert commit.index('out.saved = save') < commit.index('active = candidate;')
    assert 'if (!out.saved) {' in commit


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
    assert 'isMeasurementStale()' in pc
    stale = pc.split('bool PressureController::isMeasurementStale')[1].split('\n}\n')[0]
    assert '(millis() - _lastValidReadTime) >= TOF_STALE_MS' in stale
    # The stale check cuts the pump in update().
    upd = pc.split('void PressureController::update()')[1]
    assert 'if (isMeasurementStale()) {' in upd


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
    fn = web.split('case WEBOP_COMMIT_CONFIG', 1)[1].split('case WEBOP_RESET_CONFIG', 1)[0]
    # Never for an intermediate draft: the revision may only move once the commit
    # reports the candidate validated, saved AND activated (no pending reboot).
    assert 'else if (res.activated) {' in fn
    assert 'gmb::runtime::onConfigurationActivated();' in fn
    assert fn.index('if (res.restartRequired) {') < fn.index('else if (res.activated) {')
    # A restart-required change is saved but not activated, so it cannot notify.
    commit = read('Servo_flute_ESP32/ConfigCommit.cpp')
    restart_branch = commit.split('if (out.restartRequired) {', 1)[1].split('}', 1)[0]
    assert 'out.activated = false;' in restart_branch
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


# ---------------------------------------------------------------------------
# Audit 2026 - corrections P0/P1 (concurrence, securite actionneurs, stockage,
# reseau, authentification). Les tests comportementaux correspondants vivent
# dans tests/test_native/test_audit.cpp ; ceux-ci verrouillent l'architecture,
# qui n'est pas observable depuis les tests hote (couche AsyncTCP / LittleFS).
# ---------------------------------------------------------------------------

def test_eventqueue_exposes_no_internal_pointer():
    """P0 #1: read + remove happen under one lock; no pointer escapes the queue."""
    hdr = read('Servo_flute_ESP32/EventQueue.h')
    src = read('Servo_flute_ESP32/EventQueue.cpp')
    seq = read('Servo_flute_ESP32/NoteSequencer.cpp')
    # The pointer-returning peek() is gone from the API and from every caller.
    assert 'MidiEvent* peek()' not in hdr
    assert 'MidiEvent* EventQueue::peek' not in src
    assert '.peek()' not in seq
    # Atomic consumption primitives.
    assert 'bool tryPopDueEvent(' in hdr
    assert 'bool peekCopy(MidiEvent& out) const' in hdr
    # The due-time computation and the removal are inside the SAME critical section.
    body = src.split('bool EventQueue::tryPopDueEvent(', 1)[1].split('\n}\n', 1)[0]
    assert body.count('portENTER_CRITICAL') == 1
    assert 'popLocked();' in body
    assert body.index('portENTER_CRITICAL') < body.index('popLocked();')
    assert body.index('popLocked();') < body.index('portEXIT_CRITICAL(&_mux);\n  return true;')
    # clear() bumps an epoch so a consumer mid-burst can detect a concurrent panic.
    assert '_epoch++' in src
    assert 'uint32_t epoch() const' in hdr
    assert 'startEpoch' in seq and 'popEpoch' in seq
    # Every accessor that reads shared state takes the lock (no torn reads).
    for accessor in ('bool EventQueue::isEmpty', 'int EventQueue::getCount',
                     'unsigned long EventQueue::getReferenceTime'):
        fn = src.split(accessor, 1)[1].split('\n}\n', 1)[0]
        assert 'portENTER_CRITICAL' in fn, accessor


def test_actuator_commands_are_centralised_on_the_loop_task():
    """P0 #2: AsyncTCP/WebSocket callbacks post commands; loop() applies them."""
    cq = read('Servo_flute_ESP32/CommandQueue.h')
    im = read('Servo_flute_ESP32/InstrumentManager.cpp')
    imh = read('Servo_flute_ESP32/InstrumentManager.h')
    web = read('Servo_flute_ESP32/WebConfigurator.cpp')
    assert 'class CommandQueue' in cq
    assert 'bool postCommand(' in imh and 'void requestPanic();' in imh
    # update() drains the queue before anything else touches the hardware.
    upd = im.split('void InstrumentManager::update()', 1)[1].split('\n}\n', 1)[0]
    assert 'processCommands();' in upd
    assert upd.index('processCommands();') < upd.index('_sequencer.update()')
    # The panic is a dedicated flag: it can never be lost to a full queue, and it
    # discards commands issued before it.
    panic = read('Servo_flute_ESP32/CommandQueue.cpp').split('void CommandQueue::requestPanic()', 1)[1].split('\n}\n', 1)[0]
    assert '_panic = true;' in panic and '_count = 0;' in panic
    # The WebSocket handler never drives a controller directly any more.
    ws = web.split('void WebConfigurator::processWsMessage', 1)[1].split('void WebConfigurator::broadcastStatus', 1)[0]
    for forbidden in ('getAirflowCtrl()', 'getPressureCtrl()', 'getFanCtrl()', 'getFingerCtrl()',
                      '_instrument->noteOn(', '_instrument->noteOff(', '_instrument->allSoundOff()',
                      '_autoCal->start(', 'applyRangeResults()'):
        assert forbidden not in ws, forbidden
    assert 'postCommand(' in ws
    # A WebSocket callback must NEVER block waiting for loop(): it can hold the
    # AsyncWebSocket lock that loop() takes to broadcast, which would deadlock
    # both tasks. WS-originated operations use the non-blocking queue instead.
    assert 'runOnLoop' not in ws
    assert 'postWebOp(' in ws
    evt = web.split('void WebConfigurator::onWsEvent', 1)[1].split('void WebConfigurator::processWsMessage', 1)[0]
    assert 'runOnLoop' not in evt
    upd = web.split('void WebConfigurator::update()', 1)[1].split('\n}\n', 1)[0]
    assert 'serviceWsOps();' in upd and 'servicePendingOp();' in upd
    # cfg is never mutated from an AsyncTCP handler: the commit runs on loop().
    finalize = web.split('void WebConfigurator::handleApiConfigFinalize', 1)[1].split('\n}\n', 1)[0]
    assert 'cfg.' not in finalize
    assert 'candidate.' in finalize
    assert 'runOnLoop(op)' in finalize


def test_config_commit_is_transactional():
    """P0 #3: candidate -> normalise -> validate -> save -> atomic commit."""
    hdr = read('Servo_flute_ESP32/ConfigCommit.h')
    src = read('Servo_flute_ESP32/ConfigCommit.cpp')
    web = read('Servo_flute_ESP32/WebConfigurator.cpp')
    assert 'ConfigCommitResult commitCandidateConfig(' in hdr
    # Ordering: validate, then persist, then (and only then) activate.
    assert src.index('validateAndNormalizeConfig(candidate, &active)') < src.index('out.saved = save')
    assert src.index('out.saved = save') < src.index('active = candidate;')
    # A single assignment is the commit - no field-by-field mutation of the active config.
    assert src.count('active = candidate;') == 1
    assert 'active.' not in src.split('active = candidate;', 1)[1].split('out.activated', 1)[0]
    # The candidate is built from a copy of the active configuration.
    assert 'RuntimeConfig* candidatePtr = new RuntimeConfig(cfg);' in web
    # Persisting a candidate does not go through the global cfg.
    assert 'static bool saveFrom(const RuntimeConfig& source);' in read('Servo_flute_ESP32/ConfigStorage.h')


def test_hardware_not_ready_is_enforced_centrally():
    """P0 #4: no actuator path can bypass the hardware-ready gate."""
    im = read('Servo_flute_ESP32/InstrumentManager.cpp')
    imh = read('Servo_flute_ESP32/InstrumentManager.h')
    web = read('Servo_flute_ESP32/WebConfigurator.cpp')
    webh = read('Servo_flute_ESP32/WebConfigurator.h')
    # Single application point, with the gate at its top.
    apply_fn = im.split('void InstrumentManager::applyCommand(', 1)[1].split('\n}\n', 1)[0]
    assert 'commandDrivesActuators(cmd.type) && _hardwareInitStatus != HW_INIT_OK' in apply_fn
    assert apply_fn.index('hardware_not_ready') < apply_fn.index('switch (cmd.type)')
    assert 'static bool commandDrivesActuators(uint8_t type);' in imh
    # The web layer answers explicitly instead of silently doing nothing.
    assert 'bool hardwareReady() const;' in webh
    assert 'bool rejectIfHardwareNotReady(AsyncWebServerRequest* request);' in webh
    assert 'static bool isPhysicalWsCommand(const char* type);' in webh
    assert '"hardware_not_ready"' in web
    physical = web.split('bool WebConfigurator::isPhysicalWsCommand', 1)[1].split('\n}\n', 1)[0]
    for cmd in ('test_finger', 'test_air', 'test_angle', 'test_sol', 'test_note',
                'pump_target', 'fan_target', 'auto_cal', 'non', 'cc'):
        assert '"%s"' % cmd in physical, cmd
    # Diagnostics, configuration read/write, reset and network recovery stay reachable.
    ws = web.split('void WebConfigurator::processWsMessage', 1)[1]
    assert 'isPhysicalWsCommand(type) && !hardwareReady()' in ws
    routes = web.split('void WebConfigurator::setupRoutes', 1)[1].split('\n}\n', 1)[0]
    for open_route in ('"/api/diagnostics"', '"/api/config"', '"/api/config/reset"',
                       '"/api/wifi/connect"', '"/api/fs/format"'):
        assert open_route in routes, open_route
        assert 'rejectIfHardwareNotReady' not in routes.split(open_route, 1)[1].split('});', 1)[0]
    # The web UI surfaces the refusal.
    assert 'hardware_not_ready' in read('Servo_flute_ESP32/web_content.h')


def test_littlefs_is_never_formatted_automatically():
    """P0/P1 #5: a mount failure must never silently wipe config + MIDI files."""
    ino = read('Servo_flute_ESP32/Servo_flute_ESP32.ino')
    cs = read('Servo_flute_ESP32/ConfigStorage.cpp')
    csh = read('Servo_flute_ESP32/ConfigStorage.h')
    web = read('Servo_flute_ESP32/WebConfigurator.cpp')
    # The auto-format mount is gone from the CODE everywhere (it survives only in
    # comments explaining why, so comment lines are stripped before checking).
    def code_of(path):
        return '\n'.join(l for l in read(path).splitlines() if not l.strip().startswith('//'))
    for f in ('Servo_flute_ESP32/Servo_flute_ESP32.ino', 'Servo_flute_ESP32/ConfigStorage.cpp',
              'Servo_flute_ESP32/WebConfigurator.cpp'):
        assert 'LittleFS.begin(true)' not in code_of(f), f
    assert 'ConfigStorage::beginFilesystem()' in ino
    begin_fs = cs.split('bool ConfigStorage::beginFilesystem()', 1)[1].split('\n}\n', 1)[0]
    assert 'LittleFS.begin(false)' in begin_fs
    assert 'LittleFS.begin(true)' not in begin_fs
    assert 'FS_MOUNT_FAILED' in begin_fs
    # A failed mount keeps the actuators disabled (no InstrumentManager at all).
    assert 'bool fsMounted = ConfigStorage::beginFilesystem();' in ino
    assert 'bool bootConfigSafe = fsMounted &&' in ino
    assert ino.index('bootConfigSafe') < ino.index('instrument = new InstrumentManager();')
    # Nothing is written to an unmounted filesystem.
    for fn in ('ConfigLoadStatus ConfigStorage::loadWithStatus()',
               'bool ConfigStorage::saveFrom(', 'bool ConfigStorage::factoryReset()'):
        body = cs.split(fn, 1)[1].split('\n}\n', 1)[0]
        assert 'isFilesystemMounted()' in body, fn
    # Formatting exists, but only as an explicit, confirmed user action.
    assert 'static bool formatFilesystem();' in csh
    assert 'WEBOP_FORMAT_FS' in web
    route = web.split('"/api/fs/format"', 1)[1].split('  );', 1)[0]
    assert 'rejectIfUnauthorized' in route
    assert 'confirmation_required' in route
    assert 'doc["confirm"]' in route
    # The only call site of the formatter is that deferred operation.
    assert cs.count('LittleFS.format()') == 1
    assert web.count('ConfigStorage::formatFilesystem()') == 1


def test_vibrato_lut_is_read_as_signed():
    """P1 #6: pgm_read_byte() returned an unsigned byte and broke the negative half."""
    vm = read('Servo_flute_ESP32/VibratoMath.h')
    ac = read('Servo_flute_ESP32/AirflowController.cpp')
    assert 'namespace VibratoMath' in vm
    assert 'const int8_t SIN_LUT[SIN_LUT_SIZE]' in ac
    lut_fn = ac.split('float sinLutAt(uint8_t index)', 1)[1].split('\n}\n', 1)[0]
    assert 'return (float)SIN_LUT[index] / SIN_LUT_SCALE;' in lut_fn
    # The unsigned accessor is gone from the code (it survives only in comments
    # explaining the bug, so strip those before checking).
    code = '\n'.join(l for l in ac.splitlines() if not l.strip().startswith('//'))
    assert 'pgm_read_byte' not in code
    # The host stub keeps the real (unsigned) semantics so the bug cannot hide.
    stub = read('lib/native_stubs/include/Arduino.h')
    assert '#define pgm_read_byte(addr) ((uint8_t)(*(const uint8_t*)(addr)))' in stub
    # The frequency guard covers non-finite values too.
    fast = ac.split('float fastSin(unsigned long timeMs, float frequency)', 1)[1].split('\n}\n', 1)[0]
    assert 'isfinite(frequency)' in fast
    assert 'period == 0' in fast


def test_midi_rate_limit_never_drops_safety_messages():
    """P1 #7: channel-mode CCs bypass the limiter; CC2 coalesces instead of dropping."""
    im = read('Servo_flute_ESP32/InstrumentManager.cpp')
    imh = read('Servo_flute_ESP32/InstrumentManager.h')
    assert 'isChannelModeControlChange' in im
    guard = im.split('static inline bool isChannelModeControlChange', 1)[1].split('\n}\n', 1)[0]
    assert 'cc >= 120 && cc <= 127' in guard
    fn = im.split('void InstrumentManager::handleControlChange', 1)[1].split('\n}\n', 1)[0]
    # The channel-mode branch runs BEFORE any rate-limit bookkeeping.
    assert fn.index('isChannelModeControlChange(ccNumber)') < fn.index('_ccWindowStart')
    assert fn.index('isChannelModeControlChange(ccNumber)') < fn.index('_cc2WindowStart')
    # CC2: a silence request is always applied; otherwise the last value is kept.
    assert 'silenceRequest' in fn
    assert '_cc2Pending = true;' in fn
    assert '_cc2PendingValue = ccValue;' in fn
    assert 'bool _cc2Pending;' in imh
    assert 'serviceCc2Coalescing' in im
    upd = im.split('void InstrumentManager::update()', 1)[1].split('\n}\n', 1)[0]
    assert 'serviceCc2Coalescing(millis());' in upd
    # The safety CCs also survive a full command queue (dedicated, undroppable paths).
    post = im.split('bool InstrumentManager::postCommand(const ActuatorCommand& cmd)', 1)[1].split('\n}\n', 1)[0]
    assert 'isChannelModeControlChange(cmd.a)' in post
    assert 'requestPanic();' in post
    assert '_resetControllersRequested = true;' in post


def test_midi_upload_is_single_owner_and_validates_before_replacing():
    """P1 #8: two clients can no longer share the upload state or destroy a good file."""
    web = read('Servo_flute_ESP32/WebConfigurator.cpp')
    webh = read('Servo_flute_ESP32/WebConfigurator.h')
    # The old server-wide members are gone.
    for old in ('_uploadFile;', '_uploadSize;', '_uploadFileName;', '_uploadError;'):
        assert old not in webh, old
    assert 'struct MidiUploadSlot' in webh
    assert 'AsyncWebServerRequest* owner' in webh
    assert 'bool acquireUploadLock(AsyncWebServerRequest* request);' in webh
    assert 'void abandonStaleUpload(unsigned long now);' in webh
    assert 'UPLOAD_LOCK_TIMEOUT_MS' in read('Servo_flute_ESP32/settings.h')
    # A competing client is refused rather than interleaved.
    assert 'upload_busy' in web
    lock = web.split('bool WebConfigurator::acquireUploadLock', 1)[1].split('\n}\n', 1)[0]
    assert '_upload.owner != nullptr && _upload.owner != request' in lock
    upload = web.split('void WebConfigurator::handleMidiUpload(', 1)[1].split('\n}\n', 1)[0]
    assert '_upload.owner != request' in upload
    # Unique temp file per transfer, outside MIDI_DIR (not listed, not counted).
    assert '"/.up"' in upload and '_uploadSequence' in upload
    assert 'MIDI_FILE_PATH' not in upload
    # Name, extension, size and write failures are all validated.
    assert 'sanitizeMidiFileName' in upload
    assert 'too_large' in upload and 'write_failed' in upload
    san = web.split('static bool sanitizeMidiFileName', 1)[1].split('\n}\n', 1)[0]
    assert '".mid"' in san and '".midi"' in san
    assert '".."' in san
    # The existing file is only replaced after the upload has been fully validated.
    fin = web.split('case WEBOP_MIDI_FINALIZE', 1)[1].split('case WEBOP_MIDI_LOAD', 1)[0]
    assert 'storage_full' in fin
    assert fin.index('_player->loadFile(_upload.tmpPath.c_str())') < fin.index('LittleFS.remove(destPath)')
    assert fin.index('_player->loadFile(_upload.tmpPath.c_str())') < fin.index('LittleFS.rename(')


def test_tof_sensor_is_really_initialised_not_just_probed():
    """P1 #9: an I2C ACK at 0x29 is not a working VL53L0X."""
    tof = read('Servo_flute_ESP32/TofSensor.cpp')
    tofh = read('Servo_flute_ESP32/TofSensor.h')
    pc = read('Servo_flute_ESP32/PressureController.cpp')
    # Distinct states.
    for state in ('TOF_ABSENT', 'TOF_UNSUPPORTED', 'TOF_INIT_FAILED', 'TOF_READY', 'TOF_FAULT'):
        assert state in tofh, state
    # A real initialisation sequence, not just a start register write.
    init = tof.split('bool TofSensor::initVl53l0x()', 1)[1].split('\n}\n', 1)[0]
    assert 'VL_IDENTIFICATION_MODEL_ID' in init and 'VL53L0X_MODEL_ID' in init
    assert 'getSpadInfo(' in init
    assert 'kVl53l0xTuning' in init
    assert 'performSingleRefCalibration(0x40)' in init
    assert 'performSingleRefCalibration(0x00)' in init
    assert '_state = TOF_READY;' in init
    # Non-blocking measurement with a range-status check.
    poll = tof.split('bool TofSensor::pollMeasurement()', 1)[1].split('\n}\n', 1)[0]
    assert 'delay(' not in poll
    assert '_lastRangeStatus != 11' in poll
    # PressureController only calls the sensor usable once it is initialised.
    assert 'bool initialized = _tof.begin(' in pc
    assert '_sensorDetected = initialized;' in pc
    assert 'isSensorPresentOnBus' in read('Servo_flute_ESP32/PressureController.h')
    assert 'isSensorInitialized' in read('Servo_flute_ESP32/PressureController.h')
    # Losing the sensor still cuts the pump.
    assert 'reportTimeout(' in pc


def test_network_transitions_are_centralised_and_safe():
    """P2 #10: every AP <-> STA switch panics first and restarts services once."""
    wifi = read('Servo_flute_ESP32/WifiMidiHandler.cpp')
    wifih = read('Servo_flute_ESP32/WifiMidiHandler.h')
    assert 'void stopNetworkServices(bool notifyTransportLost);' in wifih
    assert 'void startNetworkServices();' in wifih
    stop = wifi.split('void WifiMidiHandler::stopNetworkServices', 1)[1].split('\n}\n', 1)[0]
    # The panic happens before anything is torn down.
    assert 'handleTransportLost();' in stop
    assert stop.index('handleTransportLost();') < stop.index('MDNS.end();')
    assert 'stopCaptiveDNS();' in stop
    # Services are started at most once (no repeated MDNS.begin / AppleMIDI setup).
    start = wifi.split('void WifiMidiHandler::startNetworkServices', 1)[1].split('\n}\n', 1)[0]
    assert '_mdnsStarted' in start and '_rtpMidiStarted' in start and '_captiveDnsStarted' in start
    # Both switches go through the teardown.
    for fn in ('void WifiMidiHandler::startAP()', 'void WifiMidiHandler::startSTA('):
        body = wifi.split(fn, 1)[1].split('\n}\n', 1)[0]
        assert 'stopNetworkServices(true);' in body, fn
        assert body.index('stopNetworkServices(true);') < body.index('WiFi.mode('), fn
    # forceAP() reuses startAP() instead of duplicating a partial teardown.
    force = wifi.split('void WifiMidiHandler::forceAP()', 1)[1].split('\n}\n', 1)[0]
    assert 'startAP();' in force
    assert 'WiFi.disconnect' not in force
    # Wi-Fi credentials are no longer written from the network callback.
    connect = wifi.split('void WifiMidiHandler::connectToNetwork', 1)[1].split('\n}\n', 1)[0]
    assert 'ConfigStorage::save()' not in connect


def test_hotspot_key_is_a_real_secret_not_the_mac():
    """P2 #11: the WPA2 key must not be derivable from the broadcast MAC/BSSID."""
    wifi = read('Servo_flute_ESP32/WifiMidiHandler.cpp')
    sec = read('Servo_flute_ESP32/DeviceSecrets.cpp')
    sech = read('Servo_flute_ESP32/DeviceSecrets.h')
    ap = wifi.split('void WifiMidiHandler::startAP()', 1)[1].split('\n}\n', 1)[0]
    assert 'getEfuseMac' not in ap
    assert 'flute-%06X' not in wifi
    assert 'DeviceSecrets::apPassword()' in ap
    # The key is random, persistent and at least WPA2-legal.
    assert 'esp_random()' in sec
    assert 'Preferences' in sec
    assert 'DEVICE_SECRET_LENGTH 14' in sech
    assert 'softAP(AP_SSID, apPass.c_str()' in ap
    assert 'apPass.length() < 8' in ap
    # Voluntary regeneration exists and is printed on the serial console.
    assert 'static String regenerateApPassword();' in sech
    assert 'WEBOP_REGEN_AP_PASSWORD' in read('Servo_flute_ESP32/WebConfigurator.cpp')
    assert 'printToSerial' in sec
    # The MAC is still used for the GMB instance id - that one is an identifier,
    # not a secret - but nowhere else.
    assert 'getEfuseMac' in read('Servo_flute_ESP32/gmb/GmbRuntime.cpp')


def test_web_interface_requires_authentication():
    """#12: config, actuator tests, restart, reset and MIDI files are protected."""
    web = read('Servo_flute_ESP32/WebConfigurator.cpp')
    webh = read('Servo_flute_ESP32/WebConfigurator.h')
    auth = read('Servo_flute_ESP32/WebAuth.cpp')
    ui = read('Servo_flute_ESP32/web_content.h')
    assert 'WebAuth _auth;' in webh
    routes = web.split('void WebConfigurator::setupRoutes', 1)[1].split('\n}\n', 1)[0]
    for protected in ('"/api/config", HTTP_POST', '"/api/config/reset"', '"/api/config/factory"',
                      '"/api/restart"', '"/api/fs/format"', '"/api/midi/delete"', '"/api/midi/load"',
                      '"/api/midi/list"', '"/api/wifi/connect"', '"/api/wifi/scan"'):
        seg = routes.split(protected, 1)[1].split('  );', 1)[0].split('  });', 1)[0]
        assert 'rejectIfUnauthorized' in seg, protected
    # Informational endpoints stay open.
    for public in ('"/api/status"', '"/api/diagnostics"', '"/gmb/descriptor.json"', '"/api/auth/status"'):
        seg = routes.split(public, 1)[1].split('});', 1)[0]
        assert 'rejectIfUnauthorized' not in seg, public
    # The WebSocket refuses every command until the client authenticates.
    ws = web.split('void WebConfigurator::processWsMessage', 1)[1].split('void WebConfigurator::broadcastStatus', 1)[0]
    assert 'strcmp(type, "auth") == 0' in ws
    assert '!isWsAuthenticated(client->id())' in ws
    assert ws.index('!isWsAuthenticated(client->id())') < ws.index('strcmp(type, "non")')
    # The upload rejects an unauthenticated client before writing a single byte.
    upload_route = routes.split('"/api/midi", HTTP_POST', 1)[1].split('  );', 1)[0]
    assert '_auth.validate(token, millis())' in upload_route
    # Constant-time comparisons, bounded sessions, sliding expiry.
    assert 'constantTimeEquals' in auth
    assert 'WEB_AUTH_MAX_SESSIONS' in read('Servo_flute_ESP32/WebAuth.h')
    # The UI carries the token on every request and authenticates the socket.
    assert "X-Auth-Token" in ui
    assert "t:'auth',token:AUTH.token" in ui
    assert 'loginOverlay' in ui


def test_runtime_strings_are_json_escaped():
    """#13: every network/user controllable string goes through a JSON serialiser."""
    web = read('Servo_flute_ESP32/WebConfigurator.cpp')
    wifi = read('Servo_flute_ESP32/WifiMidiHandler.cpp')
    # Hand-built config payload escapes every free-form field.
    cfg_fn = web.split('void WebConfigurator::handleApiConfig(', 1)[1].split('\n}\n', 1)[0]
    for field in ('cfg.deviceName', 'cfg.wifiSsid', 'cfg.embouchure', 'cfg.instrumentColor', 'cfg.resFormat'):
        assert 'jsonStr(%s)' % field in cfg_fn, field
        assert 'String(%s)' % field not in cfg_fn, field
    # Status broadcast and Wi-Fi scan results are fully serialised by ArduinoJson.
    bc = web.split('void WebConfigurator::broadcastStatus()', 1)[1]
    assert 'serializeJson(doc, json);' in bc
    assert 'json += ' not in bc
    scan = wifi.split('String WifiMidiHandler::getScanResultsJson', 1)[1].split('\n}\n', 1)[0]
    assert 'serializeJson(' in scan
    assert 'jsonEscape' not in wifi
    # Upload / load responses carry a file name: they are serialised too.
    assert 'JsonDocument resp;' in web.split('case WEBOP_MIDI_FINALIZE', 1)[1].split('case WEBOP_MIDI_LOAD', 1)[0]


def test_config_validation_covers_floats_and_timings():
    """#14: isfinite() + bounds on every value that could divide by zero or overflow."""
    val = read('Servo_flute_ESP32/ConfigValidator.cpp')
    csh = read('Servo_flute_ESP32/ConfigStorage.h')
    assert 'normalizeRangeFloat' in val
    assert 'isfinite(v)' in val
    for field in ('vibratoFrequencyHz', 'vibratoMaxAmplitudeDeg', 'cc2ResponseCurve'):
        assert 'normalizeRangeFloat(config.%s' % field in val, field
    for bound in ('CONFIG_MIN_VIBRATO_HZ', 'CONFIG_MAX_VIBRATO_HZ', 'CONFIG_MAX_VIBRATO_DEG',
                  'CONFIG_MIN_CC2_CURVE', 'CONFIG_MAX_CC2_CURVE'):
        assert bound in csh and bound in val, bound
    # Remaining uint16 timings are bounded.
    for field in ('minNoteIntervalForValveCloseMs', 'solenoidActivationTimeMs', 'cc2TimeoutMs',
                  'airAttackMs', 'fanIdleTimeoutMs', 'pumpStaggerMs', 'timeUnpower',
                  'sensorTargetMm', 'hallThresholdLow', 'midiStorageLimitKb'):
        assert 'normalizeRangeU16(config.%s' % field in val, field
    # CC defaults are 7-bit clamped.
    for field in ('ccVolumeDefault', 'ccExpressionDefault', 'ccModulationDefault',
                  'ccBreathDefault', 'ccBrightnessDefault', 'cc2SilenceThreshold'):
        assert 'normalizeRangeU8(config.%s, 0, MIDI_CC_MAX)' % field in val, field
    # Free-form strings are reduced to closed sets / safe formats.
    assert 'normalizeEnumString' in val and 'normalizeHexColor' in val


def test_diagnostics_report_real_state():
    """#15: /api/diagnostics must not answer "probe requires device" for known state."""
    web = read('Servo_flute_ESP32/WebConfigurator.cpp')
    fn = web.split('void WebConfigurator::handleApiDiagnostics', 1)[1].split('\n}\n', 1)[0]
    assert 'Runtime probe requires hardware' not in web
    for key in ('hardware_ready', 'pca0_detected', 'pca1_detected', 'pca1_required',
                'config_load_status', 'fs_status', 'fs_mounted', 'microphone_detected',
                'heap', 'restart_pending', 'actuator_session_active', 'dropped_commands'):
        assert '"%s"' % key in fn, key
    for key in ('initialized', 'measurement_valid', 'measurement_stale', 'present_on_bus'):
        assert '"%s"' % key in fn, key
    assert 'midi["ble"]' in fn and 'midi["rtpmidi"] ' in fn and 'midi["din"]' in fn
    # The diagnostic itself must never move an actuator.
    for forbidden in ('postCommand(', 'noteOn(', 'testAirflowAngle', 'testSolenoid',
                      'setTargetPercent', 'setSpeed('):
        assert forbidden not in fn, forbidden


def test_gmb_contract_is_preserved():
    """#18: the General-Midi-Boop surface is unchanged by the audit."""
    caps = read('Servo_flute_ESP32/gmb/Capabilities.cpp')
    sysex = read('Servo_flute_ESP32/gmb/GmbSysEx.h')
    runtime = read('Servo_flute_ESP32/gmb/GmbRuntime.cpp')
    web = read('Servo_flute_ESP32/WebConfigurator.cpp')
    assert 'instanceId' in caps or 'instanceId' in runtime
    assert '0x7D' in sysex
    assert 'onConfigurationActivated' in runtime
    # Only an activated configuration notifies; a restart-required one does not.
    commit = read('Servo_flute_ESP32/ConfigCommit.cpp')
    assert 'out.activated = true;' in commit
    assert commit.index('active = candidate;') < commit.index('out.activated = true;')
    # The HTTP descriptor route still serves the cached document.
    assert 'gmb::runtime::descriptorJson()' in web
    assert 'setHttpDescriptorAvailable(true)' in web

import re
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]

def read(path):
    return (ROOT / path).read_text(encoding='utf-8')

def code_only(text):
    """Drop // comment lines: an assertion must match real code, never a comment
    that happens to quote the same symbol."""
    return '\n'.join(l for l in text.splitlines() if not l.strip().startswith('//'))


def norm(text):
    """Normalise l'espacement AUTOUR des separateurs, sans rien retirer d'autre.

    Un audit statique qui echoue sur un simple reformatage - couper une
    affectation sur deux lignes, aligner un '=' - crie au loup : on finit par
    le contourner au lieu de l'ecouter, et il ne protege plus rien. Cette
    normalisation ne fait perdre AUCUN mordant : elle ne change ni les jetons
    ni leur ordre, donc aucun code different de celui qu'on exige ne passe
    grace a elle. Les deux cotes d'une comparaison doivent y passer.
    """
    import re
    t = re.sub(r'\s*([=;,()])\s*', r'\1', text)
    return re.sub(r'[ \t]*\n[ \t]*', '\n', t)

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
    # Le refus d'un second demarrage n'est plus une chaine en ligne : il vient
    # de CalibrationGate, pur et teste sur hote (test_fin2_calgate.cpp). On
    # verifie donc le CABLAGE, qui est la seule chose qu'un test hote ne peut
    # pas executer ici.
    assert 'calibration_busy' in read('Servo_flute_ESP32/CalibrationGate.cpp')
    assert 'calibrationStartVerdict(gate)' in web
    assert 'not_calibration_owner' in web        # non-owner stop/apply refused
    assert 'calibration_active' in web           # blocked commands + 409 config lock
    assert '409' in web                          # config POST lock status
    # Panic cancels the calibration before safing the hardware. Both now run on the
    # loop task: the cancel is a NON-DROPPABLE flag (postWebOp() could fail on a
    # full queue and leave _autoCal running after the panic), the safe state a
    # non-droppable panic request (an AsyncTCP callback must never drive actuators).
    panic = web.split('strcmp(type, "panic")', 1)[1].split('else if', 1)[0]
    assert 'requestCalibrationCancel()' in panic
    assert 'requestPanic()' in panic
    assert panic.index('requestCalibrationCancel()') < panic.index('requestPanic()')
    assert 'allSoundOff()' not in panic
    # The cancel request can no longer travel through the droppable WebOp queue.
    assert 'WEBOP_AUTOCAL_CANCEL' not in web and 'WEBOP_AUTOCAL_CANCEL' not in hdr
    assert 'postWebOp(op)' not in web.split('auto_stop', 1)[1].split('#endif', 1)[0]
    # The flag is consumed by the loop task, at the very top of update(), before
    # _autoCal->update() can re-apply anything.
    # `volatile` n'a jamais ete une garantie inter-taches : l'epingler ici
    # verrouillait le defaut. Ce qui compte est que la demande soit PRISE
    # (lue et effacee indivisiblement), ce que porte LatchedRequest.
    assert 'LatchedRequest _calCancelRequested;' in hdr
    upd = code_only(web.split('void WebConfigurator::update()', 1)[1].split('\n}\n', 1)[0])
    assert 'takeCalibrationCancel()' in upd
    assert 'cancelActiveActuatorSession();' in upd
    assert upd.index('takeCalibrationCancel()') < upd.index('_autoCal->update()')
    # P0 (2e passe): ownership is taken ONCE at the start of the calibration and
    # released ONCE at the end. The old per-loop re-take ran _sequencer.stop() on
    # every pass, closing the valve the calibrator had just opened.
    assert 'setActuatorSessionActive(true)' not in upd
    # Owner-only disconnect stops the session.
    disc = web.split('WS_EVT_DISCONNECT', 1)[1].split('WS_EVT_DATA', 1)[0]
    assert '_autoCalOwnerClientId' in disc
    assert 'requestCalibrationCancel()' in disc


def test_audit2_actuator_session_is_idempotent():
    # P0 (2e passe): the entry side effects of an actuator session (sequencer stop,
    # queue purge) must run only on a real false->true transition. They closed the
    # valve and rested the airflow servo, so repeating them mid-calibration made the
    # auto-calibration measure a silent instrument.
    im = code_only(read('Servo_flute_ESP32/InstrumentManager.cpp'))
    body = im.split('void InstrumentManager::setActuatorSessionActive')[1].split('\n}\n')[0]
    assert 'if (_actuatorSessionActive == active)' in body
    assert body.index('if (_actuatorSessionActive == active)') < body.index('_sequencer.stop()')
    # The transition also realigns the air-source tracking, otherwise the forced
    # STATE_IDLE would be read as a note end and reset the pump under the calibrator.
    assert '_prevSequencerState = STATE_IDLE;' in body
    assert '_prevNoteSounding = false;' in body
    # While a session owns the actuators, the sequencer no longer drives the air
    # source: CalibrationAirSupply does.
    upd = im.split('void InstrumentManager::update()')[1].split('\n}\n')[0]
    assert 'if (!_actuatorSessionActive) updateAirSourceFromSequencer();' in upd


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
    # Le couple (session active, proprietaire) se lit d'un seul tenant :
    # le lire a nu laissait deux clients se croire tous deux proprietaires.
    assert 'isTestOwner(client->id())' in wc
    # the old unconditional allSoundOff() on any disconnect is gone.
    # Meme raison qu'au-dessus : la deconnexion du proprietaire teste le
    # couple sous verrou au lieu de le lire a nu.
    assert 'if (isTestOwner(client->id())) {' in wc


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
    # The demand now follows BOTH the sequencer state and whether the held note is
    # really sounding: CC2 (breath) can silence a held note with no state change at
    # all, and the pump used to keep pushing at full demand against a closed valve.
    air = code_only(im).split('void InstrumentManager::updateAirSourceFromSequencer')[1].split('\n}\n')[0]
    assert '_airflowCtrl.isNoteSounding()' in air
    assert 'curState == _prevSequencerState && sounding == _prevNoteSounding' in air
    assert '(curState == STATE_POSITIONING) ||' in air
    assert '(curState == STATE_PLAYING && sounding)' in air
    assert '_prevNoteSounding = sounding;' in air
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
    assert 'postStopCommand(ACMD_NOTE_OFF, dueNote)' in web
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
    # Copier `cfg` dans le constructeur, c'etait le lire depuis AsyncTCP sans
    # verrou pendant que loop() peut le remplacer. L'allocation est desormais
    # vide et le contenu vient d'un instantane PRIS SOUS VERROU.
    assert 'RuntimeConfig* candidatePtr = new (std::nothrow) RuntimeConfig();' in web
    assert 'snapshotActiveConfig(*candidatePtr)' in web
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
    assert ino.index('bootConfigSafe') < ino.index('instrument = new (std::nothrow) InstrumentManager();')
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
    # L'installation est transactionnelle : la destination n'est plus
    # supprimee avant le remplacement, donc il n'y a plus de remove() ni de
    # rename() a ordonner - seul l'appel a fileTxInstall() doit suivre la
    # validation du contenu.
    assert fin.index('_player->loadFile(_upload.tmpPath.c_str())') < fin.index('fileTxInstall(')
    assert 'LittleFS.remove(destPath)' not in fin


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


def test_every_websocket_reply_has_a_handler_in_the_ui():
    """Toute reponse WebSocket emise par le firmware doit etre TRAITEE par l'interface.

    CE QUE CE TEST N'EST PAS : une verification de presence de texte. Les deux
    listes sont DERIVEES des sources - les types emis sont extraits de
    WebConfigurator.cpp, les types traites du repartiteur de web_content.h - et
    confrontees dans les DEUX SENS. Aucune des deux n'est recopiee ici, donc
    aucune ne peut se perimer : ajouter une reponse sans son branchement fait
    echouer ce test, et un branchement pour une reponse que plus personne
    n'emet aussi.

    LE DEFAUT QU'IL VERROUILLE, trouve par ce test meme : trois reponses
    partaient vers le navigateur qui les jetait en silence.
      - `stop_escalated` : l'ordre d'arret n'a pas pu etre mis en file et le
        serveur l'a ESCALADE en panic. Les actionneurs sont en securite, mais
        pas par le chemin demande, et tout le reste a ete coupe avec. Ne rien
        dire laissait croire a un arret ordinaire ;
      - `noise` : succes ou REFUS d'une capture de profil de bruit, avec la
        raison (no_microphone, note_playing, too_short) ;
      - `mic_reset` : resultat de la reinitialisation du micro, `status` etant
        la seule chose qui dise POURQUOI elle a echoue.

    C'est la meme famille que les trois codes d'erreur de l'audit precedent qui
    s'affichaient bruts faute de libelle : une extremite parle, l'autre
    n'ecoute pas, et rien dans la chaine ne le signale.
    """
    web = read('Servo_flute_ESP32/WebConfigurator.cpp')
    ui = read('Servo_flute_ESP32/web_content.h')

    # Les deux formes d'emission : litteral JSON a la main, et ArduinoJson.
    emitted = set(re.findall(r'\{\\"t\\":\\"([a-z_0-9]+)\\"', web))
    emitted |= set(re.findall(r'\["t"\]\s*=\s*"([a-z_0-9]+)"', web))
    # Le repartiteur de l'interface : `d.t==='<type>'`.
    handled = set(re.findall(r"d\.t===?'([a-z_0-9]+)'", ui))

    assert emitted, "aucun type de reponse extrait : l'extraction elle-meme a casse"
    assert not (emitted - handled), \
        "reponses emises par le firmware et ignorees par l'interface : %s" % sorted(emitted - handled)
    assert not (handled - emitted), \
        "branchements de l'interface pour des reponses que le firmware n'emet plus : %s" % sorted(handled - emitted)


def test_calibration_start_is_refused_while_a_manual_test_session_is_open():
    """LOT 2 : une auto-calibration ne demarre pas par-dessus un test manuel.

    LE DEFAUT : `cancelActiveActuatorSession()`, appelee au demarrage d'une
    calibration, ne touche pas `_testActive` / `_testStartTime`. Une session de
    test manuel ouverte juste avant survivait donc, et son plafond
    TEST_SESSION_MAX_MS finissait par echoir EN PLEINE MESURE :
    `endTestSession(true)` demande alors un panic, qui coupe la calibration.

    La DECISION est testee pour de vrai dans test_fin2_calgate.cpp (module pur).
    Ce qui ne peut pas l'etre, et que cette garde verrouille, c'est le CABLAGE,
    parce que WebConfigurator.cpp n'est compilable par aucun build hote.

    LE CABLAGE QUI COMPTE : `testSessionActive()` et non `isTestOwner(...)`.
    Brancher l'appartenance laisserait un SECOND navigateur lancer une
    calibration pendant le test manuel du premier - le cas a deux clients, qui
    est justement celui ou personne ne voit venir le panic.
    """
    web = read('Servo_flute_ESP32/WebConfigurator.cpp')
    hdr = read('Servo_flute_ESP32/WebConfigurator.h')
    gate = read('Servo_flute_ESP32/CalibrationGate.cpp')

    # L'accesseur existe et lit l'etat sous le verrou de session, comme ses voisins.
    assert 'bool testSessionActive() const;' in hdr
    body = web.split('bool WebConfigurator::testSessionActive() const {', 1)[1].split('\n}', 1)[0]
    assert 'portENTER_CRITICAL(&_sessionMux)' in body and 'portEXIT_CRITICAL(&_sessionMux)' in body

    # Le demarrage de calibration passe par le verdict, et lui donne la session
    # manuelle - pas son proprietaire.
    start = code_only(web.split('case WEBOP_AUTOCAL_START_RANGE:', 1)[1].split('break;\n    }', 1)[0])
    assert 'gate.manualTestActive = testSessionActive();' in start
    assert 'isTestOwner' not in start
    assert 'calibrationStartVerdict(gate)' in start
    # Et il s'arrete AVANT tout effet de bord : ni pause du lecteur, ni prise de
    # session, ni annulation, tant que le verdict n'est pas OK.
    before = start.split('calibrationStartVerdict(gate)', 1)[0]
    for side_effect in ('_player->pause()', 'cancelActiveActuatorSession()',
                        'setActuatorSessionActive(true)', '_autoCal->start('):
        assert side_effect not in before, side_effect

    # Le module refuse bien ce cas, et avec ce code-la.
    assert 'CALSTART_MANUAL_TEST_ACTIVE' in gate and 'manual_test_active' in gate


def test_every_calibration_start_error_code_has_a_label_in_the_ui():
    """Un code emis sans libelle s'affiche BRUT : acalErrText() fait `M[e]||e`.

    C'est deja arrive dans ce depot (trois codes du range finder). La liste est
    DERIVEE de CalibrationGate.cpp, donc un verdict ajoute sans son libelle
    fait echouer ce test au lieu de produire un message illisible au banc.
    """
    gate = read('Servo_flute_ESP32/CalibrationGate.cpp')
    ui = read('Servo_flute_ESP32/web_content.h')
    codes = set(re.findall(r'return "([a-z_0-9]+)";', gate))
    assert codes, "extraction des codes cassee"
    labels = ui.split('function acalErrText(', 1)[1].split('return M[e]', 1)[0]
    missing = sorted(c for c in codes if ("%s:'" % c) not in labels)
    assert not missing, "codes sans libelle dans acalErrText() : %s" % missing

    # ... ET que le chemin acal_error consulte reellement cette table. Il ne le
    # faisait PAS : il affichait `d.msg` brut, si bien que `no_microphone` - qui
    # existe depuis bien avant cette passe - s'affichait tel quel. Une table
    # complete mais jamais lue ne protege rien.
    branch = ui.split("d.t==='acal_error'", 1)[1].split('}else if', 1)[0]
    assert 'acalErrText(d.msg)' in branch


def test_websocket_operations_are_bounded_per_loop_pass():
    """LOT 3 : `serviceWsOps()` ne draine plus la file d'un seul tour.

    LE DEFAUT : `while (true) { ... executeWebOp(op); ... }`. Six places, mais
    pas six operations equivalentes - `WEBOP_MIC_RESET` passe par
    `resetMicrophone()`, qui comporte un `delay(100)` et jusqu'a ~500 ms
    d'attente I2S. Six de cette famille dans une passe retenaient `loop()` une
    duree proche du plafond du chien de garde, et repoussaient d'autant
    `InstrumentManager::update()` - l'endroit ou un ARRET ou un PANIC atteint
    reellement les actionneurs.

    La comptabilite et la borne sont testees pour de vrai dans
    test_fin2_wsops.cpp. Ce qui ne peut pas l'etre, et que cette garde
    verrouille, c'est que WebConfigurator passe bien par ce module :
    `WebConfigurator.cpp` n'est compilable par aucun build hote.
    """
    web = code_only(read('Servo_flute_ESP32/WebConfigurator.cpp'))
    hdr = code_only(read('Servo_flute_ESP32/WebConfigurator.h'))
    ring = read('Servo_flute_ESP32/WsOpRing.h')

    # Les indices ne vivent plus dans WebConfigurator : un seul proprietaire.
    for gone in ('_wsOpHead', '_wsOpTail', '_wsOpCount'):
        assert gone not in web and gone not in hdr, gone
    assert 'WsOpRing _wsOpRing;' in hdr

    body = web.split('void WebConfigurator::serviceWsOps()', 1)[1].split('\n}', 1)[0]
    # La passe est ouverte, et la sortie de boucle est la BORNE, pas un simple
    # test de file vide.
    assert '_wsOpRing.beginPass()' in body
    assert '_wsOpRing.popForPass(slot)' in body
    # Le depot passe par le meme anneau.
    post = web.split('bool WebConfigurator::postWebOp(', 1)[1].split('\n}', 1)[0]
    assert '_wsOpRing.push(slot)' in post

    # La borne doit rester PETITE : elle n'a de sens que strictement en dessous
    # de la capacite de la file (6). Une borne egale a la capacite serait un
    # drainage integral deguise.
    m = re.search(r'WS_OP_MAX_PER_PASS = (\d+);', ring)
    assert m, "constante de bornage introuvable"
    assert 1 <= int(m.group(1)) <= 2, "borne trop large : %s" % m.group(1)


def test_littlefs_format_runs_under_a_suspended_watchdog():
    """LOT 4 : le formatage LittleFS ne doit plus se faire redemarrer en plein vol.

    LE DEFAUT : `LittleFS.format()` efface ~1,9 Mo, bloquant, donc `loop()` ne
    tourne pas et `esp_task_wdt_reset()` n'est pas appele. Le chien de garde de
    tache (WATCHDOG_TIMEOUT_MS = 4000, trigger_panic) redemarrait la carte au
    milieu de l'effacement - sur le chemin de recuperation d'une carte vierge,
    c'est-a-dire au premier bring-up.

    La SEQUENCE est testee pour de vrai dans test_fin2_format.cpp (module pur,
    primitives injectees). Ce qui ne peut pas l'etre : le cablage, parce que
    `WebConfigurator.cpp` n'est compilable par aucun build hote, et
    `TaskWatchdog.cpp` n'est compile que par les deux builds ESP32 (il inclut
    esp_task_wdt.h, et un faux en-tete hote ne prouverait rien).
    """
    web = code_only(read('Servo_flute_ESP32/WebConfigurator.cpp'))
    wdt = code_only(read('Servo_flute_ESP32/TaskWatchdog.cpp'))
    settings = read('Servo_flute_ESP32/settings.h')

    handler = web.split('case WEBOP_FORMAT_FS:', 1)[1].split('break;', 1)[0]
    # Le formatage passe par la sequence, et NON plus par un appel direct.
    assert 'formatGuarded(fops)' in handler
    assert 'safeForBlockingFlashOperation()' in handler
    assert 'taskWatchdogSuspendCurrent()' in handler
    assert 'taskWatchdogResumeCurrent()' in handler
    # `formatFilesystem()` n'est appele QUE depuis la primitive confiee a la
    # sequence : un appel hors de cette lambda contournerait la garde.
    assert handler.count('ConfigStorage::formatFilesystem()') == 1
    assert 'fops.format' in handler
    # L'ancien appel nu, qui ne mettait pas /OE HIGH et ne touchait pas au chien
    # de garde, a disparu.
    assert '_instrument->allSoundOff();\n      bool ok = ConfigStorage::formatFilesystem();' not in web

    # Le CONTOURNEMENT interdit : on ne desarme pas le chien de garde pour tout
    # le monde, et on n'allonge pas son plafond.
    assert 'esp_task_wdt_delete' in wdt and 'esp_task_wdt_add' in wdt
    assert 'esp_task_wdt_deinit' not in wdt
    assert 'esp_task_wdt_init' not in wdt
    assert '#define WATCHDOG_TIMEOUT_MS 4000' in settings

    # Aucun appel ESP-IDF de chien de garde disperse dans WebConfigurator.
    assert 'esp_task_wdt' not in web


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


# =============================================================================
# Deuxieme passe d'audit (sur fcf3559) : douze constats confirmes.
# Chaque assertion ci-dessous ancre UNE correction precise, pour qu'elle ne
# puisse pas regresser silencieusement.
# =============================================================================

def test_audit2_note_off_is_never_dropped():
    """#4: un Note Off ne doit pas pouvoir etre perdu par saturation de l'anneau."""
    cqh = code_only(read('Servo_flute_ESP32/CommandQueue.h'))
    cqc = code_only(read('Servo_flute_ESP32/CommandQueue.cpp'))
    im = code_only(read('Servo_flute_ESP32/InstrumentManager.cpp'))
    # Bitmap 128 notes, hors de l'anneau.
    assert 'uint32_t _pendingNoteOff[4];' in cqh
    assert 'void requestNoteOff(uint8_t note);' in cqh
    assert 'bool takePendingNoteOff(uint8_t& note);' in cqh
    # Le bitmap est manipule sous le meme verrou que l'anneau.
    req = cqc.split('void CommandQueue::requestNoteOff')[1].split('\n}\n')[0]
    assert 'portENTER_CRITICAL(&_mux);' in req and 'portEXIT_CRITICAL(&_mux);' in req
    # Un panic (et un clear) annule les relachements en attente : tout est deja coupe.
    assert '_pendingNoteOff[i] = 0;' in cqc.split('void CommandQueue::requestPanic')[1].split('\n}\n')[0]
    # postCommand() route ACMD_NOTE_OFF vers le bitmap au lieu de l'anneau.
    post = im.split('bool InstrumentManager::postCommand(const ActuatorCommand& cmd)')[1].split('\n}\n')[0]
    assert 'cmd.type == ACMD_NOTE_OFF' in post
    assert '_commands.requestNoteOff(cmd.a);' in post
    # processCommands() les applique APRES l'anneau, et un panic reste prioritaire.
    proc = im.split('void InstrumentManager::processCommands()')[1].split('\n}\n')[0]
    assert 'takePendingNoteOff(pendingNote)' in proc
    assert proc.index('_commands.pop(cmd)') < proc.index('takePendingNoteOff(pendingNote)')
    assert 'panicPending()' in proc.split('takePendingNoteOff(pendingNote)')[1]


def test_audit2_websocket_sessions_expire():
    """#6: une WebSocket ouverte ne doit pas rester authentifiee au-dela du TTL."""
    hdr = code_only(read('Servo_flute_ESP32/WebConfigurator.h'))
    web = code_only(read('Servo_flute_ESP32/WebConfigurator.cpp'))
    # L'identifiant de client seul ne suffit plus : le jeton est memorise.
    assert '_wsAuthClients' not in hdr and '_wsAuthClients' not in web
    assert 'struct WsSession' in hdr
    assert 'char token[WEB_AUTH_TOKEN_LEN + 1];' in hdr
    assert 'void setWsAuthenticated(uint32_t clientId, const String& token);' in hdr
    assert 'void clearWsAuthentication(uint32_t clientId);' in hdr
    # isWsAuthenticated() n'est plus const : chaque commande REVALIDE le jeton,
    # ce qui applique l'expiration et fait glisser la fenetre comme pour HTTP.
    assert 'bool isWsAuthenticated(uint32_t clientId);' in hdr
    chk = web.split('bool WebConfigurator::isWsAuthenticated')[1].split('\n}\n')[0]
    assert '_auth.validate(String(_wsSessions[i].token), millis())' in chk
    # Un jeton expire libere la place au lieu de rester authentifie.
    assert '_wsSessions[i].clientId = 0;' in chk
    # Un changement de mot de passe revoque AUSSI les WebSockets.
    pwd = web.split('case WEBOP_SET_ADMIN_PASSWORD')[1].split('\n    }\n')[0]
    assert '_auth.revokeAll();' in pwd
    assert '_wsSessions[i].clientId = 0;' in pwd


def test_audit2_serial_is_always_initialised():
    """#7: les secrets d'acces sont imprimes sur le port serie, donc il doit
    toujours etre ouvert - seuls les journaux verbeux restent lies a DEBUG."""
    ino = read('Servo_flute_ESP32/Servo_flute_ESP32.ino')
    setup = ino.split('void setup()')[1].split('\nvoid loop()')[0]
    code = code_only(setup)
    # Serial.begin() est hors de tout if (DEBUG).
    assert '\n  Serial.begin(115200);' in code
    idx = code.index('Serial.begin(115200);')
    assert 'if (DEBUG)' not in code[:idx].rsplit('\n  ', 1)[-1]
    # Et il precede l'impression des secrets.
    secrets = read('Servo_flute_ESP32/DeviceSecrets.cpp')
    assert 'Serial.print' in secrets
    assert code.index('Serial.begin(115200);') < code.index('DeviceSecrets::begin();')


def test_audit2_config_reads_are_serialised_with_the_commit():
    """#8: `cfg = candidat` recopie ~5 Ko ; un lecteur AsyncTCP ne doit jamais voir
    une structure a moitie remplacee."""
    hdr = code_only(read('Servo_flute_ESP32/WebConfigurator.h'))
    web = code_only(read('Servo_flute_ESP32/WebConfigurator.cpp'))
    cch = code_only(read('Servo_flute_ESP32/ConfigCommit.h'))
    ccc = code_only(read('Servo_flute_ESP32/ConfigCommit.cpp'))
    assert 'SemaphoreHandle_t _cfgMutex;' in hdr
    assert 'bool lockConfig(uint32_t timeoutMs = WEB_CONFIG_LOCK_MS);' in hdr
    assert '_cfgMutex = xSemaphoreCreateMutex();' in web
    # Le verrou du commit n'entoure QUE l'affectation atomique : ni la validation,
    # ni l'ecriture flash (sinon un GET /api/config attendrait la flash).
    assert 'struct ConfigCommitGuard' in cch
    assign = ccc.split('RuntimeConfig previous = active;')[1].split('out.activated = true;')[0]
    assert 'guard->lock(guard->ctx)' in assign
    assert 'active = candidate;' in assign
    assert 'guard->unlock(guard->ctx)' in assign
    assert 'save(candidate)' not in assign
    # Les deux gros lecteurs AsyncTCP prennent le meme verrou et refusent plutot
    # que de bloquer la pile TCP.
    for fn in ('void WebConfigurator::handleApiConfig(',
               'void WebConfigurator::handleApiDiagnostics('):
        body = web.split(fn)[1].split('\n}\n')[0]
        assert 'lockConfig(WEB_CONFIG_LOCK_MS)' in body, fn
        assert 'config_busy' in body, fn
        assert 'unlockConfig();' in body, fn
        # Aucun hand-off vers loop() sous le verrou : pas d'interblocage possible.
        assert 'runOnLoop(' not in body, fn


def test_audit2_webop_queue_uses_a_mutex_not_a_spinlock():
    """#9: une WebOp porte des String ; la copier alloue sur le tas, ce qui est
    interdit dans une section critique."""
    hdr = code_only(read('Servo_flute_ESP32/WebConfigurator.h'))
    web = code_only(read('Servo_flute_ESP32/WebConfigurator.cpp'))
    assert '_wsOpMux' not in hdr and '_wsOpMux' not in web
    assert 'SemaphoreHandle_t _wsOpMutex;' in hdr
    for fn in ('bool WebConfigurator::postWebOp', 'void WebConfigurator::serviceWsOps'):
        body = web.split(fn)[1].split('\n}\n')[0]
        assert 'portENTER_CRITICAL' not in body, fn
        assert 'xSemaphoreTake(_wsOpMutex' in body, fn
        assert 'xSemaphoreGive(_wsOpMutex)' in body, fn


def test_audit2_vibrato_rounding_is_symmetric():
    """#10: (int16_t)(x + 0.5) tronque vers zero, donc biaise l'alternance negative."""
    ac = code_only(read('Servo_flute_ESP32/AirflowController.cpp'))
    assert 'lroundf(vibratoOffset)' in ac
    assert '(int16_t)(vibratoOffset + 0.5)' not in ac


def test_audit2_reset_all_controllers_clears_expression_state():
    """#11: CC121 doit aussi remettre l'etat runtime d'expression."""
    ach = code_only(read('Servo_flute_ESP32/AirflowController.h'))
    ac = code_only(read('Servo_flute_ESP32/AirflowController.cpp'))
    im = code_only(read('Servo_flute_ESP32/InstrumentManager.cpp'))
    assert 'void resetRuntimeState();' in ach
    body = ac.split('void AirflowController::resetRuntimeState()')[1].split('\n}\n')[0]
    for field in ('_cc2SmoothingBuffer[i]', '_cc2BufferIndex', '_cc2BufferCount',
                  '_cc2TimedOut', '_ccBreath', '_runtimeAttackMode',
                  '_runtimeAttackOffset', '_attackActive'):
        assert field in body, field
    rac = im.split('void InstrumentManager::resetAllControllers()')[1].split('\n}\n')[0]
    assert '_airflowCtrl.resetRuntimeState();' in rac
    assert '_cc2Pending = false;' in rac


def test_audit2_actuator_pins_are_safed_before_the_i2c_probe():
    """#12: un echec de sondage I2C sortait de beginSafe() sans jamais configurer
    les broches de pompe/ventilateur : grilles de MOSFET laissees flottantes."""
    imh = code_only(read('Servo_flute_ESP32/InstrumentManager.h'))
    im = code_only(read('Servo_flute_ESP32/InstrumentManager.cpp'))
    assert 'void driveConfiguredActuatorPinsInactive();' in imh
    begin = im.split('bool InstrumentManager::beginSafe()')[1].split('\n}\n')[0]
    assert 'driveConfiguredActuatorPinsInactive();' in begin
    assert begin.index('driveConfiguredActuatorPinsInactive();') < begin.index('detectPca(')
    body = im.split('void InstrumentManager::driveConfiguredActuatorPinsInactive()')[1].split('\n}\n')[0]
    assert 'pinMode(cfg.solenoidPin, OUTPUT);' in body
    assert 'pinMode(cfg.fanPin, OUTPUT);' in body
    assert 'pinMode(cfg.pumpPins[i], OUTPUT);' in body


# =============================================================================
# Chaine acoustique (PHASES 0-4). Ces verrous portent sur des choix
# d'ARCHITECTURE que les tests natifs ne peuvent pas exprimer : ils verifient
# qu'une propriete structurelle ne disparait pas discretement.
# =============================================================================

def test_audio_phase1_no_partial_frame_reaches_the_analysis():
    """A0-1 : une lecture I2S partielle ne doit plus constituer une frame."""
    aa = code_only(read('Servo_flute_ESP32/AudioAnalyzer.cpp'))
    ring = code_only(read('Servo_flute_ESP32/AudioRingBuffer.cpp'))
    # L'ancienne formule fatale a disparu du chemin d'ANALYSE. Elle reste
    # legitime dans detectMicrophone(), qui ne sonde qu'une trame de presence au
    # demarrage et n'alimente aucune mesure.
    assert '_validSamples' not in aa
    analysis = (aa.split('void AudioAnalyzer::update()')[1].split('\n}\n')[0] +
                aa.split('void AudioAnalyzer::analyzeFrame()')[1].split('\n}\n')[0])
    assert 'bytesRead' not in analysis
    # L'analyse passe par readFrame, qui refuse une frame incomplete.
    upd = aa.split('void AudioAnalyzer::update()')[1].split('\n}\n')[0]
    assert '_ring.readFrame(_frame, MIC_ANALYSIS_FRAME_SIZE, MIC_ANALYSIS_HOP_SIZE' in upd
    assert 'return;' in upd
    rf = ring.split('bool AudioRingBuffer::readFrame')[1].split('\n}\n')[0]
    assert 'if (_count < frameSize)' in rf
    assert 'bufferUnderruns++' in rf
    # Le compteur de frames n'avance QU'APRES une lecture reussie.
    assert upd.index('_ring.readFrame') < upd.index('_frameSeq++')


def test_audio_phase1_drain_is_faster_than_the_dma():
    """A0-2 : vider le DMA moins souvent qu'il ne se remplit garantit la perte."""
    s = read('Servo_flute_ESP32/settings.h')
    def val(name):
        import re
        m = re.search(r'#define\s+%s\s+([0-9]+)' % name, s)
        assert m, name
        return int(m.group(1))
    dma_samples = val('MIC_DMA_BUF_COUNT') * val('MIC_DMA_BUF_LEN')
    dma_ms = 1000.0 * dma_samples / val('MIC_SAMPLE_RATE')
    # Marge d'au moins 2x entre la profondeur du DMA et la periode de vidage.
    assert val('MIC_DRAIN_INTERVAL_MS') * 2 <= dma_ms
    # L'anneau doit contenir au moins une frame, et le hop rester dans la frame.
    assert val('MIC_RING_CAPACITY') >= val('MIC_ANALYSIS_FRAME_SIZE')
    assert 1 <= val('MIC_ANALYSIS_HOP_SIZE') <= val('MIC_ANALYSIS_FRAME_SIZE')
    # Capacite en puissance de deux (indexation par masquage).
    cap = val('MIC_RING_CAPACITY')
    assert cap & (cap - 1) == 0


def test_audio_phase1_no_analysis_timer():
    """La cadence doit s'auto-reguler sur le hop. Un minuteur plus lent que
    hop/Fe ferait deborder l'anneau en permanence : la production est fixee par
    le materiel."""
    aa = code_only(read('Servo_flute_ESP32/AudioAnalyzer.cpp'))
    upd = aa.split('void AudioAnalyzer::update()')[1].split('\n}\n')[0]
    # Le seul minuteur autorise dans update() est celui du vidage du DMA.
    assert '_lastDrain' in upd
    assert 'MIC_DRAIN_INTERVAL_MS' in upd
    # Aucun minuteur ne doit conditionner l'ANALYSE.
    assert 'AUTOCAL_FRAME_SAMPLE_MS' not in upd
    assert 'MIC_ANALYSIS_MIN_INTERVAL' not in aa


def test_audio_phase2_yin_is_unwindowed_and_non_destructive():
    """A0-3 : la fenetre appartient au chemin spectral, pas a YIN."""
    pd = code_only(read('Servo_flute_ESP32/PitchDetector.cpp'))
    pdh = code_only(read('Servo_flute_ESP32/PitchDetector.h'))
    core = pd.split('PitchResult PitchDetector::runYin')[1].split('\n}\n')[0]
    # Le coeur ne fenetre pas et ne modifie pas le signal.
    assert '_hann' not in pd and '_hann' not in pdh
    assert 'cosf' not in core
    assert 'samples[i] *=' not in core
    assert 'samples[i] -=' not in core
    # La signature est const : le tampon de l'appelant est preserve, ce qui
    # permet a l'analyse spectrale de reutiliser la meme frame sans recopie.
    assert 'PitchResult analyse(const float* samples, size_t n) const;' in pdh
    # Le raffinement est fractionnaire, plus parabolique sur tau entier.
    assert 'diffAtLag' in pd
    assert 'MIC_YIN_REFINE_ITERATIONS' in pd
    # L'ancienne methode existe UNIQUEMENT comme reference A/B, et le test A/B
    # doit reellement l'appeler.
    assert 'detectWindowed' in pdh
    t = read('tests/test_native/test_audio.cpp')
    assert 'detectWindowed' in t
    assert 'pitch_window_ab_comparison' in t
    # ...et ne doit jamais etre utilisee en production.
    assert 'detectWindowed' not in read('Servo_flute_ESP32/AudioAnalyzer.cpp')


def test_audio_phase2_expected_note_prefers_the_shortest_lag():
    """Prendre le creux le plus PROFOND serait faux : pour tout signal
    periodique d(2T) et d(3T) sont naturellement profonds, et un overblow
    passerait pour une note correcte."""
    pd = code_only(read('Servo_flute_ESP32/PitchDetector.cpp'))
    branch = pd.split('if (_expectedMidi > 0 && _expectedHz > 0.0f) {')[1].split('Chemin general')[0]
    # Rapports par lag croissant : 3*f0 d'abord, f0/2 en dernier.
    assert '3.0f, 2.0f, 1.0f, 0.5f' in branch
    # Premier qualifiant, pas le plus profond.
    assert 'break;' in branch
    assert 'best' not in branch
    # Retro-compatibilite d'IAudioSource : implementations par defaut vides.
    ias = read('Servo_flute_ESP32/IAudioSource.h')
    assert 'virtual void setExpectedMidiNote(int midi) { (void)midi; }' in ias
    assert 'virtual void clearExpectedMidiNote() {}' in ias
    # La calibration declare la note visee ET l'efface a la fin.
    ac = code_only(read('Servo_flute_ESP32/AutoCalibrator.cpp'))
    assert '_audio.setExpectedMidiNote(_expectedMidi);' in ac
    assert '_audio.clearExpectedMidiNote();' in ac.split('void AutoCalibrator::safeHardware')[1]


def test_audio_phase3_goertzel_every_frame_fft_decimated():
    """La FFT ne doit pas tourner a chaque frame : le timbre evolue bien plus
    lentement que le pitch, et elle coute bien plus cher que Goertzel."""
    sa = code_only(read('Servo_flute_ESP32/SpectralAnalyzer.cpp'))
    aa = code_only(read('Servo_flute_ESP32/AudioAnalyzer.cpp'))
    # Goertzel : pas de fenetre (on mesure une puissance a une frequence connue).
    g = sa.split('float SpectralAnalyzer::goertzelPower')[1].split('\n}\n')[0]
    assert '_window' not in g and 'windowAt' not in g
    # Au-dessus de Nyquist, refus plutot que mesure repliee.
    assert 'sampleRate * 0.5f' in g
    # FFT : fenetre de Hann LEGITIME ici.
    if 'bool SpectralAnalyzer::computeSpectrum' in sa:
        f = sa.split('bool SpectralAnalyzer::computeSpectrum')[1].split('\n}\n')[0]
        assert 'windowAt' in f
    # Decimation cablee dans l'analyseur.
    spec = aa.split('void AudioAnalyzer::analyzeSpectrum')[1].split('\n}\n')[0]
    assert 'MIC_SPECTRAL_DECIMATION' in spec
    assert '_spectralCountdown' in spec
    # Goertzel passe par le constructeur pur, appele a chaque frame.
    assert 'fillSpectral' in spec


def test_audio_phase4_features_are_measured_not_guessed():
    """Un champ ne doit jamais porter une valeur qui n'a pas ete mesuree."""
    af = code_only(read('Servo_flute_ESP32/AcousticFeatures.cpp'))
    afh = read('Servo_flute_ESP32/AcousticFeatures.h')
    # Les defauts ne suggerent aucune mesure : plancher dBFS, pas 0 dB.
    assert 'float rmsDbFS = MIC_DBFS_FLOOR;' in afh
    assert 'float peakDbFS = MIC_DBFS_FLOOR;' in afh
    assert 'bool spectralValid = false;' in afh
    # Les verdicts exigent un pitch VALIDE.
    p = af.split('void fillPitch')[1].split('\n}\n')[0]
    assert 'p.valid && p.expectedMatch' in p
    assert 'p.valid && p.octaveAbove' in p
    # Sans fondamentale fiable, les champs spectraux sont EFFACES, pas laisses.
    sp = af.split('void fillSpectral')[1].split('\n}\n')[0]
    assert 'f.spectralValid = false;' in sp
    assert sp.index('f.spectralValid = false;') < sp.index('if (frame == nullptr')
    assert 'f.spectralValid = true;' in sp
    # Le HNR est borne : un signal purement harmonique donnerait l'infini.
    assert 'MIC_HNR_MAX_DB' in sp
    # L'assemblage est PUR : aucune dependance materielle.
    for forbidden in ('i2s_', 'millis(', 'Serial.', '#include <Arduino.h>'):
        assert forbidden not in read('Servo_flute_ESP32/AcousticFeatures.cpp'), forbidden


def test_audio_web_never_streams_pcm():
    """Un ESP32-WROOM ne peut pas diffuser du PCM en continu sur WebSocket, et
    les champs spectraux non mesures doivent etre OMIS, pas repetes."""
    web = code_only(read('Servo_flute_ESP32/WebConfigurator.cpp'))
    block = web.split('String aj = "{\\"t\\":\\"audio\\"";')[1].split('_ws.textAll(aj);')[0]
    # Aucun tampon brut n'est serialise.
    for forbidden in ('_frame', 'magnitudes()', '_rawBuffer', 'AudioRingBuffer'):
        assert forbidden not in block, forbidden
    # Les champs spectraux sont conditionnes a leur validite.
    assert 'af.spectralValid' in block
    assert block.index('af.spectralValid') < block.index('af.h2Ratio')
    # Le debit reste limite.
    assert 'AUTOCAL_AUDIO_INTERVAL_MS' in web


def test_audio_phase5_filtering_runs_on_the_stream():
    """Filtrer frame par frame ferait passer chaque echantillon deux fois dans
    le filtre, les frames se recouvrant de 50 %."""
    aa = code_only(read('Servo_flute_ESP32/AudioAnalyzer.cpp'))
    drain = aa.split('void AudioAnalyzer::drainI2S()')[1].split('\n}\n')[0]
    frame = aa.split('void AudioAnalyzer::analyzeFrame()')[1].split('\n}\n')[0]
    # Le filtrage a lieu dans le vidage du flux, PAS dans l'analyse de frame.
    assert '_filters.processBlock' in drain
    assert '_filters.processBlock' not in frame
    # ...et avant l'ecriture dans l'anneau.
    assert drain.index('_filters.processBlock') < drain.index('_ring.write')
    # L'ecretage est mesure AVANT le filtrage, sur le signal brut.
    assert 'countClipped' in drain
    assert drain.index('countClipped') < drain.index('_filters.processBlock')
    # La memoire des filtres repart vierge quand le flux est relance.
    assert '_filters.configureDefaults();' in aa.split('bool AudioAnalyzer::begin()')[1]


def test_audio_phase5_filters_never_touch_the_pitch_range():
    """Une coupure qui empieterait sur la plage de detection fausserait la
    mesure au lieu de nettoyer le bruit."""
    af = read('Servo_flute_ESP32/AudioFilters.cpp')
    # Le garde est a la COMPILATION, pas seulement dans un test.
    assert 'static_assert' in af
    assert 'MIC_PITCH_MIN_HZ' in af and 'MIC_PITCH_MAX_HZ' in af
    s = read('Servo_flute_ESP32/settings.h')
    import re
    def fval(name):
        m = re.search(r'#define\s+%s\s+([0-9.]+)f' % name, s)
        assert m, name
        return float(m.group(1))
    def ival(name):
        m = re.search(r'#define\s+%s\s+([0-9.]+)f' % name, s)
        assert m, name
        return float(m.group(1))
    assert fval('MIC_FILTER_HP_HZ') <= ival('MIC_PITCH_MIN_HZ') / 2.0
    assert fval('MIC_FILTER_LP_HZ') >= ival('MIC_PITCH_MAX_HZ') * 1.2
    # Une coupure absurde rend la cellule transparente, jamais instable.
    for fn in ('void Biquad::setHighPass', 'void Biquad::setLowPass'):
        body = code_only(af).split(fn)[1].split('\n}\n')[0]
        assert 'setPassthrough();' in body
        assert 'sampleRate * 0.5f' in body


def test_audio_phase5_noise_is_per_state_and_honest():
    """Comparer une note jouee pompe en marche a un plancher mesure pompe
    arretee surestime sa qualite - c'est le cas de TOUTES les notes."""
    nm = code_only(read('Servo_flute_ESP32/NoiseModel.cpp'))
    nmh = read('Servo_flute_ESP32/NoiseModel.h')
    # Un profil par etat, pas un plancher global.
    for state in ('NOISE_AMBIENT', 'NOISE_PUMP_IDLE', 'NOISE_PUMP_MEDIUM',
                  'NOISE_PUMP_HIGH', 'NOISE_FAN_IDLE', 'NOISE_FAN_MEDIUM', 'NOISE_FAN_HIGH'):
        assert state in nmh, state
    # Aucun PCM n'est conserve : uniquement des agregats.
    assert 'float rms' in nmh and 'float bands[MIC_NOISE_BANDS]' in nmh
    assert 'samples[' not in nmh and 'float pcm' not in nmh
    # Le SNR dit quand il ne sait pas, et quand il se replie.
    snr = nm.split('SnrResult NoiseModel::snrDb')[1].split('\n}\n')[0]
    assert 'out.usedFallback = true;' in snr
    assert 'return out;' in snr            # refus quand rien n'est mesure
    assert 'MIC_SNR_MAX_DB' in snr         # borne
    assert 'if (db < 0.0f) db = 0.0f;' in snr
    # Une capture trop courte est REJETEE.
    end = nm.split('bool NoiseModel::endCapture')[1].split('\n}\n')[0]
    assert 'MIC_NOISE_MIN_FRAMES' in end
    # Le spectre doit etre RECALCULE pour une capture de bruit. analyzeSpectrum()
    # ne calcule rien sans fondamentale fiable, or une capture de bruit n'a par
    # definition pas de note : reutiliser le spectre laisse par la derniere note
    # ferait decrire cette note au profil, pas le bruit.
    aa = code_only(read('Servo_flute_ESP32/AudioAnalyzer.cpp'))
    frame = aa.split('void AudioAnalyzer::analyzeFrame()')[1].split('\n}\n')[0]
    cap = frame.split('_noise.isCapturing()')[1]
    assert '_spectral.computeSpectrum(' in cap
    assert cap.index('_spectral.computeSpectrum(') < cap.index('_noise.accumulate(')
    # ...et si le calcul echoue, on passe nullptr plutot qu'un spectre perime.
    assert 'nullptr' in cap

    # L'etat reel est declare par la couche qui le connait.
    web = code_only(read('Servo_flute_ESP32/WebConfigurator.cpp'))
    assert 'setAirSourceState(cfg.airMode' in web
    assert 'getPressureCtrl().getTargetPercent()' in web
    assert 'getFanCtrl().getSpeed()' in web
    # ...avant l'analyse, sinon le SNR porterait sur l'etat precedent.
    upd = web.split('void WebConfigurator::update()')[1].split('\n}\n')[0]
    assert upd.index('setAirSourceState') < upd.index('_audio->update();')


def test_audio_phase5_noise_capture_is_reachable_and_guarded():
    """Une fonctionnalite qu'on ne peut pas declencher n'est pas livree."""
    web = code_only(read('Servo_flute_ESP32/WebConfigurator.cpp'))
    webh = read('Servo_flute_ESP32/WebConfigurator.h')
    for op in ('WEBOP_NOISE_START', 'WEBOP_NOISE_STOP', 'WEBOP_NOISE_RESET'):
        assert op in webh, op
        assert op in web, op
    assert '"noise_cal"' in web
    # Capturer pendant qu'une note sonne mesurerait la note, pas le bruit.
    start = web.split('case WEBOP_NOISE_START')[1].split('\n    }\n')[0]
    assert 'getSequencer().getState() != STATE_IDLE' in start
    assert 'note_playing' in start
    assert 'no_microphone' in start
    # Une capture rejetee le dit, avec le minimum attendu.
    stop = web.split('case WEBOP_NOISE_STOP')[1].split('\n    }\n')[0]
    assert 'too_short' in stop
    assert 'MIC_NOISE_MIN_FRAMES' in stop
    # Le diagnostic expose l'etat du modele et avertit quand il manque.
    diag = web.split('void WebConfigurator::handleApiDiagnostics')[1].split('\n}\n')[0]
    assert 'noise_model' in diag
    assert 'No noise profile captured' in diag
    assert 'overstates quality' in diag


# ===========================================================================
# PHASES 6 et 7 - classification, note de qualite, chronometrie
#
# AudioAnalyzer.cpp depend de l'I2S : il n'entre PAS dans la compilation
# native et aucun test de tests/test_native ne l'execute. Les invariants
# ci-dessous sont donc la seule barriere automatique sur son cablage, et ils
# sont ecrits pour dire QUOI corriger, pas seulement que quelque chose cloche.
# ===========================================================================

def _analyzer_bodies():
    """Corps des fonctions d'AudioAnalyzer.cpp, commentaires retires."""
    aa = code_only(read('Servo_flute_ESP32/AudioAnalyzer.cpp'))
    out = {}
    for name in ('update', 'analyzeFrame', 'analyzeSpectrum', 'analyzeAcoustics',
                 'markMeasurementInvalid', 'resetAcousticTracking', 'drainI2S'):
        marker = 'void AudioAnalyzer::%s()' % name
        assert marker in aa, (
            "AudioAnalyzer.cpp ne definit plus %s() : les invariants des PHASES 6/7 "
            "s'appuient sur ce decoupage. Si la fonction a ete renommee, mettre a jour "
            "_analyzer_bodies() dans ce fichier." % name)
        out[name] = aa.split(marker)[1].split('\n}\n')[0]
    return aa, out


def test_audio_phase6_classification_runs_once_per_analysed_frame():
    """La classification doit tourner APRES que la frame ait recu son numero et
    son horodatage : le detecteur de couac compte les frames par leur SEQUENCE
    et refuse une sequence non contigue, la chronometrie date les siennes par
    leur horodatage. Appelee trop tot, elle travaillerait sur l'identite de la
    frame PRECEDENTE."""
    aa, fn = _analyzer_bodies()
    upd = fn['update']

    assert 'analyzeAcoustics();' in upd, (
        "AudioAnalyzer::update() n'appelle pas analyzeAcoustics() : la classification, "
        "la note de qualite et la chronometrie ne tournent alors sur AUCUNE frame. "
        "Ajouter l'appel apres _features.frameSequence / _features.timestamp.")
    assert '_features.frameSequence = _frameSeq;' in upd
    assert upd.index('_features.frameSequence = _frameSeq;') < upd.index('analyzeAcoustics();'), (
        "analyzeAcoustics() est appelee AVANT '_features.frameSequence = _frameSeq'. "
        "updateSqueak() verrait alors deux fois la meme sequence et declarerait un trou "
        "d'historique sur chaque frame (SqueakResult::historyGap), et AcousticTiming "
        "daterait la frame avec l'horodatage de la precedente. Deplacer l'appel apres "
        "la mise a jour de l'identite de la frame.")
    assert '_features.timestamp' in upd
    assert upd.index('_features.timestamp') < upd.index('analyzeAcoustics();'), (
        "analyzeAcoustics() est appelee avant '_features.timestamp' : AcousticTiming "
        "daterait chaque frame avec l'horodatage de la precedente, soit un decalage "
        "systematique d'une periode de frame sur TOUTES les mesures temporelles.")
    # Une seule fois par frame, et nulle part ailleurs.
    assert aa.count('analyzeAcoustics();') == 1, (
        "analyzeAcoustics() est appelee %d fois dans AudioAnalyzer.cpp. Elle fait AVANCER "
        "des machines a etats (couac, chronometrie) : deux appels pour une seule frame "
        "compteraient cette frame deux fois." % aa.count('analyzeAcoustics();'))

    acou = fn['analyzeAcoustics']
    assert 'AcousticQuality::classify(_features, ctx, &_squeak)' in acou, (
        "analyzeAcoustics() doit appeler AcousticQuality::classify(_features, ctx, &_squeak). "
        "Passer nullptr a la place de &_squeak rendrait missingSqueakHistory vrai en "
        "permanence : aucun couac ne serait jamais detecte.")
    assert '_classification = ' in acou, (
        "Le resultat de classify() n'est pas range dans _classification : getClassification() "
        "rendrait toujours un verdict vide.")
    assert 'computeAcousticQuality' in acou and '_quality = ' in acou, (
        "analyzeAcoustics() doit ranger AcousticQuality::computeAcousticQuality(...) dans "
        "_quality, sinon getQualityScore() ne rend jamais de note.")
    # La respiration est REPRISE de la classification, qui l'a deja calculee.
    assert '_classification.breathiness' in acou, (
        "computeAcousticQuality() doit recevoir _classification.breathiness. classify() a "
        "deja calcule la respiration et l'a rangee la ; la recalculer couterait huit "
        "Goertzel de 1024 points par frame pour un resultat identique.")
    assert 'computeBreathiness' not in acou, (
        "analyzeAcoustics() appelle computeBreathiness() alors que classify() vient de la "
        "calculer et de la ranger dans _classification.breathiness. C'est huit Goertzel de "
        "1024 points (~8 % de YIN) payes deux fois par frame, 62,5 fois par seconde, pour "
        "exactement le meme nombre : computeBreathiness est une fonction pure.")
    # L'attaque n'est PAS inventee a partir d'une duree en millisecondes.
    assert 'AQ_ATTACK_NOT_MEASURED' in acou, (
        "computeAcousticQuality() doit recevoir AQ_ATTACK_NOT_MEASURED tant qu'aucune "
        "conversion duree -> note 0..1 n'a ete definie et justifiee. Fabriquer cette "
        "valeur ici ferait passer un reglage de gout pour une mesure ; la sentinelle "
        "laisse la composante absente et QualityScore::weightUsed le dit.")


def test_audio_phase6_context_is_honest_about_what_was_measured():
    """AcousticContext est le seul endroit du firmware ou l'on peut declarer
    fraiche une mesure qui ne l'est pas sans que rien ne le detecte."""
    aa, fn = _analyzer_bodies()
    acou = fn['analyzeAcoustics']
    # norm() : ces assertions portent sur des JETONS, pas sur une mise en page.
    # Couper une affectation sur deux lignes ne change pas ce que le code fait.
    nacou, naa = norm(acou), norm(aa)

    assert norm('ctx.fftFresh = _features.fftValid;') in nacou, (
        "ctx.fftFresh doit valoir _features.fftValid, le drapeau que fillSpectral() pose "
        "quand la FFT a REELLEMENT tourne sur cette frame. Toute autre source (un compteur "
        "de decimation recopie ici, spectralValid, une constante) finirait par diverger de "
        "ce que la chaine a fait.")
    # L'invariant central de la PHASE 6 : jamais de fraicheur inconditionnelle.
    import re
    bad = [rhs.strip() for rhs in re.findall(r'fftFresh\s*=([^;]+);', aa)
           if ' '.join(rhs.split()) != '_features.fftValid']
    assert not bad, (
        "AcousticContext::fftFresh est pose a '%s' dans AudioAnalyzer.cpp. Le centroide et "
        "la platitude spectrale ne sont recalcules qu'une frame sur MIC_SPECTRAL_DECIMATION "
        "(64 ms) et ne sont PAS effaces entre-temps : les declarer frais a chaque frame fait "
        "juger la respiration et la brillance sur une mesure perimee. Seul "
        "_features.fftValid dit la verite." % bad[0])
    assert norm('ctx.fftFresh = true') not in naa and norm('ctx.fftFresh = 1') not in naa

    assert norm('ctx.stabilityMeasured = _features.stabilityValid;') in nacou, (
        "ctx.stabilityMeasured doit valoir _features.stabilityValid (propage depuis "
        "PitchResult::stabilityValid). pitchStability vaut 0 tant que l'historique n'est pas "
        "rempli ET 0 pour une note franchement instable : sans ce drapeau, chaque debut de "
        "note serait classe ACOUSTIC_UNSTABLE.")
    assert norm('stabilityMeasured = true') not in naa, (
        "ctx.stabilityMeasured est force a vrai : la stabilite serait lue comme mesuree "
        "alors que l'historique de pitch n'est pas encore rempli.")

    assert norm('ctx.frame = _frame;') in nacou, (
        "ctx.frame doit pointer le PCM de la frame courante. Sans lui, evaluateOverblow() "
        "rend valid=false et l'etat ACOUSTIC_OVERBLOW devient inatteignable : son critere "
        "spectral se mesure a la note VISEE et a son octave, alors que les harmoniques deja "
        "rangees dans _features sont ancrees sur la frequence DETECTEE - donc sur l'octave "
        "elle-meme pendant un overblow.")
    assert norm('ctx.frameSize = MIC_ANALYSIS_FRAME_SIZE;') in nacou
    assert norm('ctx.sampleRate = (float)MIC_SAMPLE_RATE;') in nacou
    assert norm('ctx.expectedMidi = _expectedMidi;') in nacou, (
        "ctx.expectedMidi doit venir de la note visee declaree a l'analyseur : sans elle, "
        "ni fausse note ni overblow ne peuvent etre juges (missingExpectedNote).")


def test_audio_phase6_a_stale_verdict_is_invalidated_not_kept():
    """Un verdict perime n'est pas 'un peu moins vrai' : il est faux. Le laisser
    en place ferait lire un etat acoustique d'il y a au moins
    MIC_FRAME_STALE_MS comme s'il decrivait l'instant present."""
    aa, fn = _analyzer_bodies()
    stale = fn['markMeasurementInvalid']

    for what, why in (
        ('_classification = AcousticClassification();',
         "l'etat acoustique et tous les drapeaux 'missing' resteraient ceux de la derniere "
         "frame reussie"),
        ('_quality = QualityScore();',
         "getQualityScore().valid resterait vrai sur une mesure perimee"),
        ('_squeak.reset();',
         "l'historique de brillance se compte en FRAMES et _frameSeq n'avance que sur les "
         "frames ANALYSEES : apres un trou de flux la sequence reste contigue alors que le "
         "temps a saute, et le detecteur ne peut donc pas voir ce trou tout seul"),
    ):
        assert what in stale, (
            "AudioAnalyzer::markMeasurementInvalid() ne fait pas '%s' : %s." % (what, why))

    # La chronometrie, elle, n'est PAS effacee par un trou d'acquisition.
    assert '_timing.reset()' not in stale, (
        "markMeasurementInvalid() remet AcousticTiming a zero. Son cycle est pilote par les "
        "ordres d'actionneur, pas par l'analyse, et elle possede ses propres plafonds : "
        "l'effacer sur un simple trou d'acquisition perdrait la note en cours de mesure.")

    reset = fn['resetAcousticTracking']
    assert '_squeak.reset();' in reset and '_pitch.resetTracking();' in reset, (
        "resetAcousticTracking() doit remettre a zero le detecteur de couac ET l'historique "
        "de pitch (contrat d'interface) : l'historique de brillance decrit la note "
        "PRECEDENTE et son etendue de pitch traverserait les deux notes.")
    assert '_pitch.setExpectedMidiNote(_expectedMidi);' in reset, (
        "resetTracking() efface aussi la note visee du detecteur : resetAcousticTracking() "
        "doit la restaurer, sinon changer de note reviendrait a ne plus en viser aucune et "
        "la levee d'ambiguite d'octave de YIN serait perdue.")
    assert '_classification = AcousticClassification();' in reset and '_quality = QualityScore();' in reset, (
        "resetAcousticTracking() doit aussi invalider les verdicts : ils portaient sur la "
        "note precedente.")
    assert '_timing' not in reset, (
        "resetAcousticTracking() touche a _timing. Le contrat d'interface l'interdit : le "
        "cycle de la chronometrie commence a noteCommanded() et se termine a noteReleased(), "
        "tous deux emis par la chaine d'actionneurs. L'effacer depuis le chemin d'ANALYSE "
        "perdrait la note en cours de chronometrage.")


def test_audio_frame_path_never_allocates():
    """Regle du projet : pas d'allocation dynamique dans le chemin audio. Une
    allocation par frame, 62,5 fois par seconde, fragmente le tas d'un ESP32
    jusqu'a l'echec - et l'echec arrive des heures plus tard, ailleurs."""
    import re
    aa, fn = _analyzer_bodies()
    path = '\n'.join(fn[k] for k in ('update', 'analyzeFrame', 'analyzeSpectrum',
                                     'analyzeAcoustics', 'markMeasurementInvalid',
                                     'resetAcousticTracking', 'drainI2S'))
    forbidden = (
        (r'\bnew\b', "operator new"),
        (r'\bmalloc\s*\(', "malloc()"),
        (r'\bcalloc\s*\(', "calloc()"),
        (r'\brealloc\s*\(', "realloc()"),
        (r'\bstrdup\s*\(', "strdup()"),
        (r'\bString\b', "une String Arduino (elle alloue a la construction ET a chaque "
                        "concatenation)"),
        (r'\bstd::vector\b', "std::vector"),
        (r'\bstd::string\b', "std::string"),
    )
    for pattern, what in forbidden:
        hit = re.search(pattern, path)
        assert hit is None, (
            "Le chemin d'analyse par frame d'AudioAnalyzer.cpp utilise %s. Ce code tourne "
            "62,5 fois par seconde sur un ESP32 sans allocateur temps reel : utiliser un "
            "membre de taille fixe (comme _frame, _chunk ou _features) a la place." % what)
    # Les tampons de travail restent des membres dimensionnes a la compilation.
    h = code_only(read('Servo_flute_ESP32/AudioAnalyzer.h'))
    assert 'float _frame[MIC_ANALYSIS_FRAME_SIZE];' in h
    assert 'SqueakDetector _squeak;' in h, (
        "L'etat persistant de la detection de couac doit etre un MEMBRE d'AudioAnalyzer. "
        "Une variable statique cachee dans AcousticQuality ferait interferer deux "
        "instances - deux flutes, ou deux tests - par un etat partage invisible.")


def test_audio_phase7_timing_is_fed_by_the_analysis_and_commanded_by_the_actuators():
    """Le flux est a SENS UNIQUE : l'analyse alimente la chronometrie, la chaine
    d'actionneurs lui donne les instants d'ordre, et aucune decision ne depend
    de ce qu'elle rend."""
    aa, fn = _analyzer_bodies()
    acou = fn['analyzeAcoustics']
    h = code_only(read('Servo_flute_ESP32/AudioAnalyzer.h'))

    assert '_timing.update(AcousticTiming::fromFeatures(_features));' in acou, (
        "analyzeAcoustics() doit alimenter la chronometrie une fois par frame analysee, par "
        "_timing.update(AcousticTiming::fromFeatures(_features)). Sans ce flux, aucune "
        "mesure temporelle n'existe : toutes les durees resteraient invalides.")
    assert aa.count('_timing.update(') == 1, (
        "_timing.update() est appelee %d fois : la machine a etats temporelle avancerait "
        "plusieurs fois sur une seule frame." % aa.count('_timing.update('))

    # Les accesseurs du contrat d'interface, mot pour mot.
    for decl in ('const AcousticClassification& getClassification() const',
                 'const QualityScore&           getQualityScore() const',
                 'const BreathinessResult&      getBreathiness() const',
                 'const SqueakResult&           getSqueak() const',
                 'const char*                   getAcousticStateName() const',
                 'void                          resetAcousticTracking()',
                 'AcousticTiming&               timing()',
                 'const AcousticTiming&         timing() const'):
        assert decl in h, (
            "AudioAnalyzer.h ne declare plus '%s'. Ces signatures sont fixees par le contrat "
            "d'interface et appelees par la chaine d'actionneurs et par la couche web : les "
            "changer casse la compilation de deux autres modules." % decl)

    # SENS UNIQUE : l'analyse n'emet aucun ordre de chronometrie.
    for hook in ('noteCommanded', 'airCommanded', 'valveOpened', 'noteReleased'):
        assert hook not in aa, (
            "AudioAnalyzer.cpp appelle %s(). Les instants d'ORDRE appartiennent a la chaine "
            "d'actionneurs, qui seule sait QUAND l'ordre est parti ; les fabriquer depuis "
            "l'analyse daterait l'ordre au moment ou son effet est observe, ce qui rendrait "
            "toute latence nulle par construction." % hook)


def test_audio_phase7_pitch_validity_has_a_single_definition():
    """fromFeatures() reconstruisait le critere de validite du pitch parce
    qu'AcousticFeatures ne portait pas le verdict du detecteur. Il le porte."""
    at = code_only(read('Servo_flute_ESP32/AcousticTiming.cpp'))
    body = at.split('TimingFrame AcousticTiming::fromFeatures')[1].split('\n}\n')[0]
    assert 'out.pitchValid = f.pitchValid;' in body, (
        "AcousticTiming::fromFeatures() doit propager le verdict du detecteur "
        "(out.pitchValid = f.pitchValid). Reconstruire le critere a partir de la confiance "
        "suppose - au lieu de l'exprimer - que pitchHz n'est renseigne que dans "
        "[MIC_PITCH_MIN_HZ, MIC_PITCH_MAX_HZ] : une frequence repliee hors plage avec une "
        "bonne confiance passerait pour un pitch.")
    assert 'MIC_YIN_CONFIDENCE_MIN' not in body, (
        "fromFeatures() compare a nouveau la confiance a MIC_YIN_CONFIDENCE_MIN : deux "
        "definitions de 'pitch fiable' dans la meme chaine finissent toujours par diverger. "
        "Le seul critere est AcousticFeatures::pitchValid, rempli par fillPitch() depuis "
        "PitchResult::valid.")
    # Et l'assemblage renseigne bien ce champ, sinon la propagation rendrait
    # tous les pitchs invalides.
    af = code_only(read('Servo_flute_ESP32/AcousticFeatures.cpp'))
    fill = af.split('void fillPitch')[1].split('\n}\n')[0]
    assert 'f.pitchValid = p.valid;' in fill, (
        "AcousticFeatureBuilder::fillPitch() ne propage plus PitchResult::valid vers "
        "AcousticFeatures::pitchValid : AcousticTiming::fromFeatures() declarerait alors "
        "TOUS les pitchs invalides et pitchStabilizationTime ne serait jamais mesure.")


def test_audio_phase6_web_status_publishes_the_weight_with_the_score():
    """Un score de qualite publie sans son poids ment sur ce qu'il mesure : 0,82
    pondere a 1,00 et 0,82 pondere a 0,60 ne decrivent pas le meme son."""
    web = code_only(read('Servo_flute_ESP32/WebConfigurator.cpp'))
    assert 'doc["audio"].to<JsonObject>()' in web, (
        "Le JSON de statut n'expose plus d'objet 'audio' : les assertions d'exposition des "
        "PHASES 6 et 7 s'appuient dessus.")
    block = web.split('doc["audio"].to<JsonObject>()')[1].split('\n}\n')[0]

    for field in ('acoustic_state', 'acoustic_classified', 'quality_score',
                  'quality_weight_used', 'quality_valid', 'breathiness',
                  'breathiness_valid', 'hnr_db', 'hnr_is_spectral'):
        assert 'a["%s"]' % field in block, (
            "Le JSON de statut n'expose pas a[\"%s\"], exige par le contrat d'interface "
            "(section Exposition web)." % field)
    assert block.index('a["quality_score"]') < block.index('a["quality_weight_used"]') , (
        "quality_weight_used doit etre publie A COTE de quality_score, pas ailleurs dans "
        "le document : un lecteur qui ne trouve pas le poids a l'endroit du score le croira "
        "absent et comparera des scores incomparables.")

    # De quoi la classification a ete privee.
    assert 'a["missing"]' in block, (
        "Les drapeaux 'missing*' doivent aller dans un sous-objet a[\"missing\"] : un etat "
        "'good' obtenu faute d'avoir pu mesurer le pitch n'est pas un etat 'good'.")
    missing = block.split('a["missing"]')[1]
    for flag in ('pitch', 'snr', 'spectrum', 'expected_note', 'stability',
                 'squeak_history', 'snr_fallback'):
        assert '"%s"' % flag in missing, (
            "a[\"missing\"] n'expose pas \"%s\", exige par le contrat d'interface." % flag)


def test_audio_phase7_web_timing_measures_all_carry_their_validity():
    """Un attack de 0 ms sans son drapeau se lit comme une attaque instantanee,
    alors qu'il signifie 'jamais mesuree'. C'est la valeur la plus trompeuse
    possible ici, et c'est exactement le defaut a ne pas reintroduire."""
    import re
    web = code_only(read('Servo_flute_ESP32/WebConfigurator.cpp'))
    block = web.split('doc["audio"].to<JsonObject>()')[1].split('\n}\n')[0]

    assert 'a["timing"]' in block, (
        "Le JSON de statut n'expose pas a[\"timing\"] : la derniere note chronometree "
        "(PHASE 7) reste invisible depuis l'interface.")
    tm = block.split('a["timing"]')[1]

    measures = ('command_to_sound', 'air_to_sound', 'attack', 'pitch_stabilization', 'release')

    # L'emetteur d'une duree : une fonction qui prend un TimingMeasure et ecrit
    # SES DEUX champs. Sans elle, la forme doit etre inline - mais elle doit
    # exister sous l'une ou l'autre forme.
    emitter = re.search(r'\b(\w+)\s*\([^)]*const\s+TimingMeasure&[^)]*\)\s*\{(.*?)\n\}',
                        web, re.S)
    if emitter:
        name, body = emitter.group(1), emitter.group(2)
        assert '"valid"' in body and '"ms"' in body, (
            "%s() publie une duree de chronometrie sans ecrire A LA FOIS \"valid\" et \"ms\" : "
            "une duree sans son drapeau se lit comme une mesure." % name)
        for m in measures:
            assert re.search(r'%s\s*\([^;]*"%s"' % (re.escape(name), m), tm), (
                "La mesure \"%s\" n'est pas publiee par %s() : elle ne porterait donc pas sa "
                "validite. Le contrat exige {\"valid\":bool,\"ms\":float} pour chacune des "
                "cinq durees." % (m, name))
    else:
        for m in measures:
            assert '"%s"' % m in tm, (
                "La mesure \"%s\" n'est pas publiee dans a[\"timing\"]." % m)
            sub = tm.split('"%s"' % m)[1][:400]
            assert '"valid"' in sub and '"ms"' in sub, (
                "La mesure \"%s\" est publiee sans {\"valid\":bool,\"ms\":float}." % m)

    # Aucune duree publiee comme un nombre NU.
    for m in measures:
        assert not re.search(r'\["%s"\]\s*=' % m, tm), (
            "La duree \"%s\" est affectee directement dans le JSON : elle part alors sans son "
            "drapeau de validite, et un 0 ms 'jamais mesure' devient une latence nulle." % m)

    # Le contexte de la note, sans lequel les durees ne se relisent pas.
    for field in ('outcome', 'baseline_valid', 'has_last'):
        assert '"%s"' % field in tm, (
            "a[\"timing\"] n'expose pas \"%s\", exige par le contrat d'interface." % field)


def _analyzer_functions():
    """Toutes les fonctions membres d'AudioAnalyzer.cpp, {nom: corps}."""
    import re
    aa = code_only(read('Servo_flute_ESP32/AudioAnalyzer.cpp'))
    starts = [(m.group(1), m.start()) for m in
              re.finditer(r'^[A-Za-z_][\w:<>*&\s]*?\bAudioAnalyzer::(\w+)\s*\(', aa, re.M)]
    out = {}
    for i, (name, pos) in enumerate(starts):
        end = starts[i + 1][1] if i + 1 < len(starts) else len(aa)
        out.setdefault(name, '')
        out[name] += aa[pos:end]
    return out


def test_audio_every_path_that_stops_acquisition_invalidates_the_verdicts():
    """update() sort des sa premiere ligne quand l'analyseur est inactif ou non
    initialise : le plafond d'obsolescence (MIC_FRAME_STALE_MS) ne s'y applique
    donc JAMAIS. Toute fonction qui arrete ou reinitialise l'acquisition doit
    invalider elle-meme, sinon le dernier verdict reste publie pour toujours -
    l'API continuerait a rendre l'etat acoustique et le score d'une note
    d'avant l'arret comme s'ils venaient d'etre mesures."""
    import re
    fns = _analyzer_functions()

    for name, why in (
        ('begin', "un flux qui redemarre heriterait des verdicts et de l'historique de "
                  "brillance du flux precedent"),
        ('end', "l'analyseur arrete continuerait de publier le verdict de sa derniere frame"),
        ('resetMicrophone', "apres un reset depuis l'interface web (WEBOP_MIC_RESET), l'API "
                            "republierait l'etat acoustique et le score d'AVANT le reset, et la "
                            "ligne de base du detecteur de couac apprise avant servirait a juger "
                            "les frames d'apres ; de plus resetMicrophone() remet _frameTimestamp "
                            "a zero, ce qui DESARME le plafond d'obsolescence"),
        ('setActive', "mettre l'analyse en pause figerait le dernier verdict pour toute la duree "
                      "de la pause"),
    ):
        assert name in fns, (
            "AudioAnalyzer.cpp ne definit pas %s() : cet invariant de cycle de vie s'appuie "
            "sur ce decoupage." % name)
        assert 'markMeasurementInvalid();' in fns[name], (
            "AudioAnalyzer::%s() n'invalide pas les mesures rangees : %s. Appeler "
            "markMeasurementInvalid()." % (name, why))

    # setActive() ne doit plus etre le simple drapeau qu'il etait : la
    # declaration inline du header ne peut pas invalider.
    h = code_only(read('Servo_flute_ESP32/AudioAnalyzer.h'))
    assert 'void setActive(bool active) override;' in h, (
        "setActive() doit etre declare dans AudioAnalyzer.h et defini dans le .cpp : sous sa "
        "forme inline '{ _active = active; }' il ne peut pas invalider les verdicts a la mise "
        "en pause.")
    sa = fns['setActive']
    assert 'if (_active == active) return;' in sa, (
        "setActive() doit agir sur les TRANSITIONS. Plusieurs appelants reposent l'etat a une "
        "valeur qu'il a deja (WebConfigurator le recalcule a chaque evenement) : agir sur la "
        "valeur effacerait l'historique de pitch et la ligne de base de brillance a chaque "
        "passage, et la stabilite ne serait jamais mesuree.")
    assert 'resetAcousticTracking();' in sa, (
        "setActive(true) doit repartir d'un historique vierge : ce que le detecteur de couac "
        "et l'historique de pitch avaient appris decrit un autre moment de jeu, separe par une "
        "pause de duree inconnue.")

    # GARDE GENERALE : toute AUTRE fonction qui coupe l'acquisition devra en
    # faire autant. Ce test echoue alors sur le nouveau chemin, par son nom.
    for name, body in fns.items():
        stops = re.search(r'_active\s*=\s*(false|active)\s*;', body) or '_initialized = false;' in body
        if not stops:
            continue
        assert 'markMeasurementInvalid();' in body, (
            "AudioAnalyzer::%s() arrete ou reinitialise l'acquisition (_active / _initialized) "
            "sans invalider les mesures rangees. update() sortant immediatement dans cet etat, "
            "le plafond d'obsolescence ne s'appliquera jamais : appeler "
            "markMeasurementInvalid()." % name)


# ===========================================================================
# VERROUS DE STRUCTURE - ordre, multiplicite, coexistence obligatoire
#
# Une assertion qui ne verifie que la PRESENCE d'un litteral detecte la
# suppression d'une ligne, jamais sa neutralisation : un `return;` premature,
# un `if (false)`, une affectation ecrasee plus bas la laissent toutes passer.
# Les verrous ci-dessous encodent des invariants qu'un texte present ne peut
# pas simuler : un ordre entre deux instructions, un nombre d'appelants, une
# portee syntaxique partagee.
#
# Ils protegent deux defauts CONSTATES sur ce fichier, et reproduits :
#   - resetAcousticTracking() n'avait qu'un seul appelant (setActive(true)),
#     alors que trois endroits promettaient qu'un changement de note l'appelait
#     aussi ;
#   - markMeasurementInvalid() laissait _level et _rms intacts, donc
#     /api/diagnostics publiait le niveau et le verdict d'ecretage de la
#     derniere frame analysee apres end(), apres setActive(false) et apres un
#     resetMicrophone() rate.
# ===========================================================================

def _fn_body(fns, name):
    """Corps d'une fonction rendue par _analyzer_functions(), signature exclue."""
    assert name in fns, (
        "AudioAnalyzer.cpp ne definit plus %s() : ce verrou s'appuie sur ce "
        "decoupage. Si la fonction a ete renommee, mettre a jour ce test." % name)
    src = fns[name]
    return src[src.index('{') + 1:].split('\n}\n')[0]


def _braced(text, marker):
    """Bloc { ... } qui suit `marker`, accolades APPARIEES.

    Verifier que deux cles JSON partent 'sous la meme garde' demande de
    connaitre la portee, pas de chercher une sous-chaine : deux cles peuvent se
    suivre dans le fichier et vivre dans deux `if` differents. A n'employer que
    sur des gardes dont le corps ne contient pas d'accolade en chaine.
    """
    i = text.index(marker)
    j = text.index('{', i)
    depth = 0
    for k in range(j, len(text)):
        if text[k] == '{':
            depth += 1
        elif text[k] == '}':
            depth -= 1
            if depth == 0:
                return text[j + 1:k]
    raise AssertionError("bloc non ferme apres %r" % marker)


def test_audio_a_declared_note_change_resets_the_acoustic_tracking():
    """MULTIPLICITE + ORDRE. resetAcousticTracking() n'avait qu'UN appelant -
    setActive(true) - alors que le contrat d'interface, son propre commentaire
    et l'en-tete promettaient qu'un changement de note l'appelait. Effet
    reproduit sur un legato montant d'une octave : la premiere frame de la
    nouvelle note est comparee a la ligne de base de brillance de l'ancienne,
    le detecteur publie ACOUSTIC_SQUEAK - troisieme dans l'ordre de priorite,
    donc il masque tout ce qui suit - pendant six frames sur une note propre.
    """
    import re
    fns = _analyzer_functions()

    # --- MULTIPLICITE : une fonction de remise a zero sans appelant est morte.
    callers = sorted(n for n in fns
                     if n != 'resetAcousticTracking'
                     and 'resetAcousticTracking(' in _fn_body(fns, n))
    for expected in ('setActive', 'setExpectedMidiNote', 'clearExpectedMidiNote'):
        assert expected in callers, (
            "AudioAnalyzer::%s() n'appelle pas resetAcousticTracking(). Appelants "
            "trouves : %s. Declarer une note visee, la retirer, ou reprendre apres une "
            "pause sont les trois SEULS signaux de changement de note dont cette classe "
            "dispose ; en perdre un laisse la ligne de base de brillance et l'historique "
            "de pitch d'une note servir a juger la suivante." % (expected, callers or 'aucun'))
    assert len(callers) >= 3, (
        "resetAcousticTracking() n'a plus que %d appelant(s) : %s. Le defaut d'origine "
        "etait exactement celui-la - un seul appelant pour une fonction que trois "
        "endroits de la documentation declaraient appelee a chaque changement de note."
        % (len(callers), callers or 'aucun'))

    # --- ORDRE : on ne detecte pas une transition apres avoir ecrase l'ancienne
    # valeur, et on ne restaure pas la note visee avant de l'avoir posee.
    for name in ('setExpectedMidiNote', 'clearExpectedMidiNote'):
        body = _fn_body(fns, name)
        read = re.search(r'(_expectedMidi\s*[!=<>]=|[!=<>]=\s*_expectedMidi)', body)
        write = re.search(r'(?<![\w.])_expectedMidi\s*=(?!=)', body)
        reset = re.search(r'resetAcousticTracking\s*\(', body)
        assert write, (
            "AudioAnalyzer::%s() n'affecte plus _expectedMidi : ce verrou s'appuie sur "
            "cette fonction comme point de declaration de la note visee." % name)
        assert read, (
            "AudioAnalyzer::%s() ne compare plus rien a _expectedMidi avant de l'ecrire. "
            "La remise a zero doit se faire sur TRANSITION : un appelant qui redeclare la "
            "meme note a chaque passage - c'est ce que font les appelants de setActive() - "
            "effacerait sinon l'historique de pitch a chaque tour, et la stabilite ne "
            "serait JAMAIS mesuree (MIC_PITCH_HISTORY frames sont necessaires)." % name)
        assert read.start() < write.start(), (
            "AudioAnalyzer::%s() lit _expectedMidi APRES l'avoir ecrit : le test de "
            "transition compare alors la nouvelle valeur a elle-meme et est toujours "
            "faux, donc resetAcousticTracking() ne serait plus jamais appelee." % name)
        assert reset and write.start() < reset.start(), (
            "Dans AudioAnalyzer::%s(), resetAcousticTracking() est appelee AVANT que "
            "_expectedMidi ne recoive sa nouvelle valeur. Elle restaure la note visee du "
            "detecteur depuis ce membre : appelee trop tot, elle y remet l'ANCIENNE note "
            "et la levee d'ambiguite d'octave de YIN vise la note precedente." % name)


def test_audio_no_published_measurement_survives_an_invalidation():
    """COEXISTENCE, deduite du code et non d'une liste ecrite a la main : tout
    membre qu'analyzeFrame() renseigne a chaque frame ET qu'un accesseur publie
    doit etre remis a zero par markMeasurementInvalid().

    Sans cet invariant, _level et _rms ont survecu a end(), a setActive(false)
    et a un resetMicrophone() rate : /api/diagnostics rendait encore rms_dbfs,
    peak_dbfs, clipping et clipping_ratio de la derniere frame analysee, et le
    verdict ACTIF "Microphone input is clipping" avec, pendant qu'acoustic_state
    rendait deja "unclassified" - les deux moities du meme bloc JSON ne
    decrivaient pas le meme instant. Le verrou s'etend tout seul au prochain
    champ ajoute.
    """
    import re
    fns = _analyzer_functions()
    h = code_only(read('Servo_flute_ESP32/AudioAnalyzer.h'))
    frame = _fn_body(fns, 'analyzeFrame')
    stale = _fn_body(fns, 'markMeasurementInvalid')

    assign = r'(?<![\w.])(_\w+)\s*=(?!=)'
    written = []
    for m in re.finditer(assign, frame):
        if m.group(1) not in written:
            written.append(m.group(1))
    assert '_level' in written and '_rms' in written, (
        "analyzeFrame() ne renseigne plus _level / _rms : ce verrou deduit du code la "
        "liste des mesures a invalider, et cette liste vient de la.")

    # Un membre est PUBLIE s'il sort par un accesseur du header.
    published = [m for m in written if re.search(r'return\s+%s\s*[;.]' % m, h)]

    # Seule exception, et elle est motivee : `_noiseCaptureFinished` ne dit pas
    # ce que mesure la frame courante, il dit qu'une capture de bruit s'est
    # terminee D'ELLE-MEME au plafond de duree. endNoiseCapture() s'en sert pour
    # rapporter comme un succes un profil reellement range. L'effacer ici
    # perdrait une capture legitimement terminee.
    EXEMPT = {'_noiseCaptureFinished'}

    for member in published:
        if member in EXEMPT:
            continue
        cleared = (re.search(r'(?<![\w.])%s\s*=(?!=)' % member, stale)
                   or ('%s.reset()' % member) in stale)
        assert cleared, (
            "AudioAnalyzer::analyzeFrame() renseigne %s a chaque frame et un accesseur du "
            "header le publie, mais markMeasurementInvalid() ne le remet pas a zero. "
            "update() sort des sa premiere ligne quand l'analyseur est inactif : le "
            "plafond d'obsolescence ne s'appliquera donc JAMAIS, et cette mesure restera "
            "publiee apres end(), apres setActive(false) et apres un resetMicrophone() "
            "dont le begin() echoue, comme si elle venait d'etre prise. Ajouter sa remise "
            "a zero dans markMeasurementInvalid(), ou - si elle doit survivre - "
            "l'inscrire dans EXEMPT avec la raison." % member)

    # ANTI-NEUTRALISATION. Ces deux fonctions sont des remises a zero
    # inconditionnelles : elles n'ont rien a rendre. Un `return` y est, par
    # construction, un moyen de sauter la fin de la liste - exactement la
    # neutralisation qu'une assertion de presence ne verrait pas.
    for name in ('markMeasurementInvalid', 'resetAcousticTracking'):
        body = _fn_body(fns, name)
        assert not re.search(r'\breturn\b', body), (
            "AudioAnalyzer::%s() contient un `return`. Cette fonction remet a zero une "
            "LISTE de champs ; un retour anticipe en laisse une partie intacte, et les "
            "champs sautes continueront d'etre publies comme des mesures. Si une "
            "condition est vraiment necessaire, garder l'instruction concernee, pas le "
            "reste de la fonction." % name)

    # ORDRE. resetTracking() efface aussi la note visee du detecteur : la
    # restaurer AVANT reviendrait a ne pas la restaurer du tout.
    for name in ('markMeasurementInvalid', 'resetAcousticTracking'):
        body = _fn_body(fns, name)
        wipe = body.index('_pitch.resetTracking()')
        restore = body.index('_pitch.setExpectedMidiNote(_expectedMidi)')
        assert wipe < restore, (
            "Dans AudioAnalyzer::%s(), la note visee est rendue au detecteur AVANT "
            "_pitch.resetTracking(), qui l'efface juste apres. Le detecteur repart donc "
            "sans note visee : YIN perd la levee d'ambiguite d'octave deterministe et un "
            "overblow cesse d'etre detecte COMME overblow." % name)


def test_audio_ws_push_never_separates_a_value_from_its_scale_or_weight():
    """COEXISTENCE DE PORTEE. Trois couples de la poussee WebSocket n'ont de
    sens qu'ensemble : les separer ne degrade pas l'information, il la rend
    fausse.

    - "hnr" recouvre DEUX echelles (mesure spectrale ou approximation Goertzel
      a quatre raies) qui different de 31,88 dB sur la MEME note et classent
      donc les notes a l'envers ; "hnr_sp" dit laquelle. La FFT ne tournant
      qu'une frame sur MIC_SPECTRAL_DECIMATION, la cle alterne entre les deux a
      15,6 Hz.
    - "br" est une moyenne ponderee dont deux composantes sur trois n'existent
      qu'une frame sur MIC_SPECTRAL_DECIMATION : "brw" descend a 0,30, et seul
      lui distingue "pas de souffle" de "presque rien de mesure".
    - "stab" vaut 0 pour "pas encore mesure" AUTANT que pour "tres instable" :
      il ne part que sous af.stabilityValid.
    """
    web = code_only(read('Servo_flute_ESP32/WebConfigurator.cpp'))
    push = web.split('{\\"t\\":\\"audio\\"')[1].split('_ws.textAll(aj);')[0]

    def key(k):
        return '\\"%s\\":' % k

    for guard, pair, why in (
        ('if (af.spectralValid)', ('hnr', 'hnr_sp'),
         "l'echelle du HNR - 31,88 dB d'ecart entre les deux - serait perdue et le "
         "chiffre nu classerait les notes a l'envers"),
        ('if (br.valid)', ('br', 'brw'),
         "le poids de la respiration serait perdu, et 0,00 a poids 0,30 se lirait "
         "comme 0,00 a poids 1,00"),
    ):
        scope = _braced(push, guard)
        for k in pair:
            assert key(k) in scope, (
                "La poussee WebSocket n'emet pas \"%s\" sous %s : %s." % (k, guard, why))
            assert push.count(key(k)) == 1, (
                "\"%s\" est emis %d fois dans la poussee WebSocket. Une seule emission, "
                "sous la garde qui la qualifie : une seconde, ailleurs, echapperait a la "
                "garde et c'est exactement ce que ce verrou interdit."
                % (k, push.count(key(k))))
        assert scope.index(key(pair[0])) < scope.index(key(pair[1])), (
            "\"%s\" doit etre emis JUSTE APRES \"%s\", pas ailleurs dans le message : un "
            "lecteur qui ne trouve pas le qualificatif a l'endroit de la valeur le croira "
            "absent." % (pair[1], pair[0]))

    stab = _braced(push, 'if (af.stabilityValid)')
    assert key('stab') in stab, (
        "\"stab\" n'est plus emis sous if (af.stabilityValid). AcousticFeatures.h le dit : "
        "0 signifie 'pas encore mesure' AUTANT que 'tres instable'. Il faut "
        "MIC_PITCH_HISTORY frames pour que le chiffre veuille dire quelque chose, alors "
        "que la poussee part toutes les AUTOCAL_AUDIO_INTERVAL_MS : la PREMIERE poussee "
        "de chaque note porterait \"stab\":0.00 et chaque debut de note serait lu comme "
        "un defaut.")
    assert push.count(key('stab')) == 1, (
        "\"stab\" est emis %d fois : une emission hors garde republierait la valeur non "
        "mesuree que la garde sert a taire." % push.count(key('stab')))


def test_audio_diagnostics_keeps_each_measure_next_to_what_qualifies_it():
    """ORDRE + COEXISTENCE DE PORTEE dans /api/diagnostics, sur le modele de
    quality_score / quality_weight_used.

    breathiness_weight_used doit etre publie ENTRE la valeur et son drapeau :
    l'ecart est plus grand que pour la qualite (le poids descend a 0,30 contre
    0,75) et valid reste vrai dans les deux cas.

    hnr_valid part TOUJOURS ; hnr_db et hnr_is_spectral seulement mesures. Quand
    spectralValid est faux, fillSpectral() remet harmonicToNoiseRatio a 0.0f, et
    ce "hnr_db": 0 etait indistinguable d'une vraie mesure Goertzel autour de
    0 dB - la reference documentee d'une note timbree sur cette echelle vaut
    -0,06 dB.
    """
    web = code_only(read('Servo_flute_ESP32/WebConfigurator.cpp'))
    diag = web.split('void WebConfigurator::handleApiDiagnostics')[1]
    block = diag.split('doc["audio"].to<JsonObject>()')[1].split('\n}\n')[0]

    for f in ('a["breathiness"]', 'a["breathiness_weight_used"]', 'a["breathiness_valid"]'):
        assert f in block, (
            "/api/diagnostics n'expose plus %s dans le bloc audio." % f)
    assert (block.index('a["breathiness"]')
            < block.index('a["breathiness_weight_used"]')
            < block.index('a["breathiness_valid"]')), (
        "breathiness_weight_used doit etre publie ENTRE breathiness et "
        "breathiness_valid, pas ailleurs dans le document. La respiration est une "
        "moyenne ponderee de trois composantes dont deux n'existent qu'une frame sur "
        "MIC_SPECTRAL_DECIMATION : sur une note tenue immobile la valeur alterne entre "
        "une mesure pleine et 0,00 a 62,5 Hz avec valid=true dans les DEUX cas. Un "
        "lecteur qui ne trouve pas le poids a l'endroit de la valeur le croira absent et "
        "comparera des mesures incomparables.")

    # hnr_valid dehors, hnr_db et hnr_is_spectral dedans : la portee EST
    # l'invariant, la presence ne suffit pas.
    scope = _braced(block, 'if (feat.spectralValid)')
    assert 'a["hnr_valid"]' in block and 'a["hnr_valid"]' not in scope, (
        "a[\"hnr_valid\"] doit etre publie INCONDITIONNELLEMENT : c'est lui qui dit que "
        "hnr_db n'a pas ete mesure. Le mettre sous la garde le fait disparaitre "
        "exactement quand il est necessaire, et l'absence des trois cles redevient "
        "indistinguable d'un bloc audio tronque.")
    for f in ('a["hnr_db"]', 'a["hnr_is_spectral"]'):
        assert f in scope, (
            "%s doit etre publie SOUS if (feat.spectralValid). Sans mesure spectrale, "
            "fillSpectral() remet harmonicToNoiseRatio a 0.0f : publie tel quel, ce "
            "\"hnr_db\": 0 est indistinguable d'une vraie mesure Goertzel autour de 0 dB, "
            "soit la valeur de reference documentee d'une note timbree sur cette "
            "echelle (-0,06 dB). Et une valeur sans son echelle ment de 31,88 dB." % f)
        assert block.count(f) == 1, (
            "%s est publie %d fois : une seconde emission hors garde republierait le 0 "
            "que la garde sert a taire." % (f, block.count(f)))

    # Les deux modes de panne de la chronometrie. Les compter sans jamais les
    # publier revient a ne pas les compter.
    tm = block.split('a["timing"]')[1]
    for f in ('rejected_frames', 'rejected_events'):
        assert 'tm["%s"]' % f in tm, (
            "a[\"timing\"] n'expose pas \"%s\". C'est l'un des deux SEULS temoins des "
            "modes de panne de la chronometrie : sans lui, un releve vide ou immobile ne "
            "se distingue pas d'une absence de jeu, et un cablage d'appels errone "
            "(rejected_events) passe pour un defaut de jeu." % f)


def _firmware_translation_units():
    """Les sources reellement compilees pour l'ESP32.

    `web_content.h` est exclu : il ne contient pas de C++ mais l'interface web
    en JavaScript, dans une chaine litterale brute, ou `new Headers(...)`,
    `new Set(...)` et `new WebSocket(...)` sont parfaitement normaux.
    """
    import re
    root = ROOT / 'Servo_flute_ESP32'
    skip = {'web_content.h'}
    out = []
    for pattern in ('*.cpp', '*.h', '*.ino', 'gmb/*.cpp', 'gmb/*.h'):
        for path in sorted(root.glob(pattern)):
            if path.name in skip:
                continue
            text = path.read_text(encoding='utf-8')
            # code_only() ne retire que les lignes `//`. Il faut aussi retirer
            # les blocs /* */ : les en-tetes de ce depot sont abondamment
            # commentes et plusieurs parlent d'une "new configuration".
            text = re.sub(r'/\*.*?\*/', ' ', text, flags=re.DOTALL)
            # ... et les commentaires de FIN de ligne, que code_only() laisse
            # passer : `bool applied;  // new angles are persisted in cfg` etait
            # compte comme une allocation.
            text = re.sub(r'//[^\n]*', '', text)
            out.append((path.relative_to(ROOT).as_posix(), code_only(text)))
    return out


def test_p2_no_bare_new_in_firmware_sources():
    """Toute allocation du firmware passe par `new (std::nothrow)`.

    Un `new` ORDINAIRE qui echoue ne rend pas nullptr, et la carte redemarre.
    ATTENTION au mecanisme, que cette docstring a d'abord donne FAUX : le
    firmware n'est pas compile sans exceptions. Le builder Arduino ajoute
    `-fexceptions -fno-rtti` apres les options du projet - meme mecanique que le
    `-std=gnu++11` documente dans platformio.ini. Un `new` nu qui echoue LEVE
    donc ; rien n'attrape dans ce firmware, l'exception remonte hors de loop(),
    `std::terminate()` appelle `abort()`, et la carte panique. Le resultat est
    le meme, la raison n'est pas celle qui etait ecrite ici.

    Ce n'est pas un detail : c'est parce que les exceptions sont ACTIVES que
    `GmbSysExService::setSnapshot()` peut attraper un echec de rendu et garder
    l'ancien descripteur. Le build ESP32 de la CI est ce qui tient cette
    affirmation honnete - si les exceptions venaient a etre coupees, ce
    `try`/`catch` ne compilerait plus, bruyamment.

    Plusieurs allocations etaient
    pourtant suivies d'une gestion d'echec soignee - un test de nullite, un mode
    degrade, un code HTTP 500 - qui ne pouvait donc jamais s'executer. Le cas le
    plus net etait `new RuntimeConfig(cfg)` dans WebConfigurator, suivi
    immediatement de `if (candidatePtr == nullptr)` : une protection ecrite,
    relue en revue, et morte.

    Un `new` nu est donc interdit ici, non par style, mais parce qu'il rend
    INATTEIGNABLE le code de repli ecrit juste en dessous - et qu'un repli
    inatteignable est indiscernable, a la lecture, d'un repli qui marche.
    """
    import re
    offenders = []
    # `new` suivi d'un type, sans placement : on ignore `new (std::nothrow)` et
    # les autres formes a placement, qui sont explicites par construction.
    bare = re.compile(r'\bnew\s+(?!\()[A-Za-z_]')
    # EXCLUSION RAISONNEE, par categorie et non par liste de lignes (une liste
    # de lignes se perime en silence) : la construction d'un conteneur ou d'une
    # chaine de la bibliotheque standard. `new std::string(GmbDescriptor::toJson(...))`
    # en est le seul cas ici, et y mettre std::nothrow donnerait une FAUSSE
    # assurance : toJson() construit deja une std::string, donc elle a deja
    # alloue - et abandonne en cas d'echec - avant que ce `new` ne s'execute. La
    # rendre "sure" ferait croire a un chemin de repli qui n'existe pas en
    # amont. Ces allocations sont hors du chemin d'actionneur : elles servent le
    # descripteur General-Midi-Boop.
    stdlib = re.compile(r'\bnew\s+std::')
    for rel, src in _firmware_translation_units():
        for i, line in enumerate(src.splitlines(), 1):
            if bare.search(line) and not stdlib.search(line):
                offenders.append(f'{rel}:{i}: {line.strip()}')
    assert not offenders, (
        "Allocations sans std::nothrow - la gestion d'echec ecrite en dessous ne "
        "pourra pas se declencher :\n  " + "\n  ".join(offenders)
    )


def test_p2_config_lock_fails_closed_when_the_mutex_could_not_be_created():
    """lockConfig() ne doit plus confondre "rien a serialiser" et "plus rien
    pour serialiser".

    `xSemaphoreCreateMutex()` alloue, donc elle peut rendre NULL sur un tas
    epuise. lockConfig() rendait alors `true` - c'est-a-dire annoncait un verrou
    acquis - et laissait ecrire `cfg` sans aucune protection pendant que les
    taches AsyncTCP tournaient. Un verrou qui ment est pire qu'un verrou absent :
    l'appelant cesse de se mefier. C'est aussi exactement ce que
    commitCandidateConfig() interroge via son garde.
    """
    src = code_only(read('Servo_flute_ESP32/WebConfigurator.cpp'))
    hdr = code_only(read('Servo_flute_ESP32/WebConfigurator.h'))
    assert '_cfgMutexFailed' in hdr, (
        "L'echec de CREATION du mutex n'est plus distingue de son absence avant "
        "begin() : lockConfig() ne peut plus echouer en fermeture."
    )
    assert norm('_cfgMutexFailed = (_cfgMutex == nullptr);') in norm(src)
    body = src.split('bool WebConfigurator::lockConfig', 1)[1].split('\n}', 1)[0]
    assert 'return !_cfgMutexFailed;' in body, (
        "lockConfig() ne rend plus false quand la creation du mutex a echoue."
    )
    assert 'if (_cfgMutex == nullptr) return true;' not in body, (
        "Le retour inconditionnel `true` sur mutex absent est revenu."
    )


# --- Section 11 : une validation materielle ne se decrete pas --------------

HW_NOT_TESTED = 'NOT TESTED — requires hardware'
# Convention de preuve : pour qu'une ligne quitte NOT TESTED, la colonne
# Comments doit porter `EXECUTED <AAAA-MM-JJ> <sha7+>`. Ni la date ni le SHA ne
# sont verifiables par la CI - c'est normal, ce n'est pas leur role. Leur role
# est qu'on ne puisse pas changer un statut SANS ECRIRE sur quoi et quand
# l'essai a tourne. Un statut qu'on ne peut pas retracer ne vaut rien.
HW_EVIDENCE = __import__('re').compile(r'EXECUTED\s+\d{4}-\d{2}-\d{2}\s+[0-9a-f]{7,40}')


def _hardware_matrix_rows():
    """[(id, statut, commentaires)] pour chaque ligne des tableaux de la
    matrice - il y en a plusieurs, et ils doivent tous obeir a la regle."""
    rows = []
    for line in read('Servo_flute_ESP32/docs/HARDWARE_TEST_MATRIX.md').splitlines():
        line = line.strip()
        if not line.startswith('|') or not line.endswith('|'):
            continue
        cells = [c.strip() for c in line.strip('|').split('|')]
        if len(cells) != 8:
            continue
        if cells[0] in ('ID',) or set(cells[0]) <= set('- '):
            continue
        rows.append((cells[0], cells[6], cells[7]))
    return rows


def test_hardware_matrix_never_claims_pass_without_recorded_evidence():
    """Aucun statut ne quitte NOT TESTED sans preuve tracable.

    Rien de ce depot n'a tourne sur un ESP32 avec des peripheriques physiques.
    Une case passee a PASS apres une passe purement logicielle est la pire sortie
    possible de ce travail : elle transforme une lacune CONNUE en confiance
    infondee, et c'est precisement ce qu'une longue campagne de corrections rend
    tentant - on a beaucoup travaille, donc on a envie que ce soit valide.

    Le test n'interdit pas de marquer PASS. Il interdit de le faire sans dire
    QUAND et SUR QUEL FIRMWARE, via `EXECUTED <AAAA-MM-JJ> <sha>` dans les
    commentaires de la ligne.
    """
    rows = _hardware_matrix_rows()
    assert len(rows) >= 70, (
        f"Seulement {len(rows)} lignes lues dans la matrice : l'analyse ne mord "
        "plus sur le tableau reel, donc elle ne protege plus rien."
    )
    faulty = [
        f'{rid}: statut "{status}" sans preuve EXECUTED dans les commentaires'
        for rid, status, comments in rows
        if status != HW_NOT_TESTED and not HW_EVIDENCE.search(comments)
    ]
    assert not faulty, (
        "Statuts materiels revendiques sans trace d'execution :\n  "
        + "\n  ".join(faulty)
        + "\nAjoutez `EXECUTED <AAAA-MM-JJ> <sha>` dans les commentaires de la "
          "ligne, ou laissez " + HW_NOT_TESTED + "."
    )


def test_hardware_matrix_is_still_entirely_unexecuted():
    """Etat REEL, pin par la CI plutot que par la memoire de quelqu'un.

    Tant que ce test passe, la reponse a "est-ce que ca a ete valide sur
    materiel ?" est non, en totalite - et elle est verifiee, pas affirmee. Le
    jour ou un essai est reellement mene, ce test echoue : c'est voulu. Il faut
    alors venir ici, constater que la ligne porte bien sa preuve, et ajuster le
    compte en connaissance de cause.
    """
    rows = _hardware_matrix_rows()
    executed = [(rid, status) for rid, status, _c in rows if status != HW_NOT_TESTED]
    assert not executed, (
        "Des lignes ne sont plus NOT TESTED : "
        + ', '.join(f'{r} -> {s}' for r, s in executed)
        + ". Si l'essai a vraiment eu lieu sur un ESP32 avec ses peripheriques, "
          "mettez ce test a jour DELIBEREMENT. Sinon, c'est une regression de "
          "l'honnetete du depot."
    )


def test_p1_wifi_commit_never_treats_saved_as_activated():
    """`saved` ne veut pas dire "la RAM est a jour".

    Depuis que le commit refuse d'ecrire la configuration active sans le verrou,
    un commit peut finir `saved=true, activated=false`. Le chemin WiFi lisait
    UNIQUEMENT `saved` : il repondait "Connecting...", lancait la bascule reseau,
    et surtout laissait `cfg` porter les ANCIENS identifiants.

    La consequence n'est pas cosmetique. Le prochain POST /api/config construit
    son candidat a partir de `cfg` : il aurait reecrit en flash les anciens
    identifiants, effacant en silence ceux que l'utilisateur venait
    d'enregistrer. Une perte de donnees, declenchee par un simple timeout de
    verrou.
    """
    src = code_only(read('Servo_flute_ESP32/WebConfigurator.cpp'))
    case = src.split('case WEBOP_WIFI_CONNECT:', 1)[1].split('case WEBOP_', 1)[0]
    assert 'res.activated' in case, (
        "Le chemin WiFi ne regarde plus `activated` : il retraite `saved` comme "
        "s'il voulait dire que la configuration active a change."
    )
    assert norm('if (res.saved && res.activated && _wirelessManager)') in norm(case), (
        "La bascule reseau n'est plus conditionnee a l'activation : elle "
        "partirait sur des identifiants que la configuration active ignore."
    )
    assert 'scheduleControlledRestart();' in case, (
        "Aucun redemarrage n'est programme quand la flash est en avance sur la "
        "RAM : la divergence resterait ouverte jusqu'au prochain ecrasement."
    )
    # La reponse doit porter l'information, sinon le client croit la bascule faite.
    assert 'resp["restart_required"] = true;' in case


def test_p1_commit_response_exposes_activation_not_just_application():
    """Le client doit pouvoir distinguer "sauvegardee" de "activee" sans le deduire."""
    src = code_only(read('Servo_flute_ESP32/WebConfigurator.cpp'))
    commit = src.split('case WEBOP_COMMIT_CONFIG', 1)[1].split('case WEBOP_', 1)[0]
    assert 'resp["activated"] = res.activated;' in commit


def test_p1_autocal_apply_commits_under_the_configuration_lock():
    """Les deux `apply*` du calibrateur passent le verrou de configuration.

    Depuis que AutoCalibrator emprunte le commit transactionnel, il remplace la
    configuration active - 5132 octets - par le meme chemin que la voie web.
    Sans garde, ce remplacement se fait HORS verrou pendant qu'une tache
    AsyncTCP peut lire `cfg` : precisement le dechirement que le verrou existe
    pour empecher.

    Ce test existe a cause de la forme de l'API : les deux parametres sont
    OPTIONNELS, donc les oublier compile sans un avertissement et sans le
    moindre symptome visible. Un defaut qui ne se manifeste qu'a la course
    entre taches ne sera pas trouve par la relecture ; il doit etre epingle
    ici.
    """
    src = code_only(read('Servo_flute_ESP32/WebConfigurator.cpp'))
    assert norm('_autoCal->applyResults(_instrument, &cfgGuard)') in norm(src), (
        "applyResults() est appelee sans garde : le commit ecrirait la "
        "configuration active hors verrou."
    )
    assert norm('_autoCal->applyRangeResults(_instrument, &cfgGuard)') in norm(src), (
        "applyRangeResults() est appelee sans garde."
    )
    assert 'applyResults();' not in src and 'applyRangeResults();' not in src, (
        "Un appel sans argument subsiste : il compilerait, et perdrait le verrou."
    )
    # Persiste sans etre actif = divergence RAM/flash : elle doit etre resolue,
    # pas seulement constatee.
    assert src.count('scheduleControlledRestart();') >= 6
    assert 'ra.saved && !ra.applied' in src
    assert 'ap.saved && !ap.applied' in src

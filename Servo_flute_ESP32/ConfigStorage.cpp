#include "ConfigStorage.h"
#include "ConfigPersist.h"   // remplacement atomique .tmp/.bak, pur et teste sur hote

#include <new>   // std::nothrow : une allocation ratee doit rendre nullptr, pas lever

static ConfigLoadStatus s_lastLoadStatus = CONFIG_DEFAULTS;
static String s_lastLoadError;
static FilesystemStatus s_fsStatus = FS_NOT_MOUNTED;
static String s_fsError;
#include <ArduinoJson.h>
#include <LittleFS.h>

// Instance globale
RuntimeConfig cfg;


ConfigLoadStatus ConfigStorage::lastLoadStatus() { return s_lastLoadStatus; }
const String& ConfigStorage::lastLoadError() { return s_lastLoadError; }

/*******************************************************************************
 * Systeme de fichiers - montage fail-safe
 ******************************************************************************/

bool ConfigStorage::beginFilesystem() {
  // formatOnFail = false : un echec de montage ne doit JAMAIS effacer la
  // partition. Perdre /config.json silencieusement ferait redemarrer
  // l'instrument sur une configuration par defaut qui ne correspond pas
  // forcement au cablage reel (mode d'air, broches de pompe, canaux PCA).
  if (LittleFS.begin(false)) {
    s_fsStatus = FS_MOUNTED;
    s_fsError = "";
    return true;
  }

  // Deuxieme tentative : un premier echec peut venir d'une initialisation de
  // peripherique encore en cours. Toujours sans formatage.
  if (LittleFS.begin(false)) {
    s_fsStatus = FS_MOUNTED;
    s_fsError = "";
    return true;
  }

  s_fsStatus = FS_MOUNT_FAILED;
  s_fsError = "LittleFS mount failed (no automatic format); manual recovery required";
  if (DEBUG) {
    Serial.println("ERREUR: LittleFS - montage impossible. Mode recovery : actionneurs desactives.");
    Serial.println("        Formatage volontaire requis (POST /api/fs/format) pour repartir a neuf.");
  }
  return false;
}

FilesystemStatus ConfigStorage::filesystemStatus() { return s_fsStatus; }
const String& ConfigStorage::filesystemError() { return s_fsError; }
bool ConfigStorage::isFilesystemMounted() {
  return s_fsStatus == FS_MOUNTED || s_fsStatus == FS_FORMATTED;
}

bool ConfigStorage::formatFilesystem() {
  // Action DESTRUCTIVE et VOLONTAIRE uniquement (mode recovery). Efface la
  // configuration et tous les fichiers MIDI.
  if (DEBUG) Serial.println("DEBUG: LittleFS - formatage volontaire demande");
  LittleFS.end();
  if (!LittleFS.format()) {
    s_fsError = "LittleFS format failed";
    return false;
  }
  if (!LittleFS.begin(false)) {
    s_fsStatus = FS_MOUNT_FAILED;
    s_fsError = "LittleFS still unmountable after format";
    return false;
  }
  s_fsStatus = FS_FORMATTED;
  s_fsError = "";
  return true;
}

/*******************************************************************************
 * Adaptateurs LittleFS -> FsRenameOps
 *
 * La sequence de remplacement (et sa recuperation au demarrage) vit dans
 * ConfigPersist.cpp, sans LittleFS ni ArduinoJson : c'est ce qui la rend
 * executable sur hote contre un faux systeme de fichiers qui echoue a volonte.
 * Ici, on ne fait que brancher les vraies operations.
 ******************************************************************************/
static bool littleFsExists(void*, const char* path) { return LittleFS.exists(path); }
static bool littleFsRemove(void*, const char* path) { return LittleFS.remove(path); }
static bool littleFsRename(void*, const char* from, const char* to) {
  return LittleFS.rename(from, to);
}

static FsRenameOps littleFsRenameOps() {
  FsRenameOps ops;
  ops.exists = &littleFsExists;
  ops.remove = &littleFsRemove;
  ops.rename = &littleFsRename;
  ops.ctx = nullptr;   // LittleFS est un singleton global : pas d'etat a porter
  return ops;
}

bool ConfigStorage::load() {
  return loadWithStatus() == CONFIG_LOADED;
}

ConfigLoadStatus ConfigStorage::loadWithStatus() {
  // D'abord initialiser les defauts
  initDefaults();
  s_lastLoadStatus = CONFIG_DEFAULTS;
  s_lastLoadError = "";

  // Systeme de fichiers non monte : on ne peut ni lire ni ecrire. On NE fait PAS
  // semblant de tourner sur les defauts - c'est signale comme erreur de stockage
  // pour que le boot laisse les actionneurs desactives.
  if (!isFilesystemMounted()) {
    s_lastLoadStatus = CONFIG_STORAGE_ERROR;
    s_lastLoadError = filesystemError().length() ? filesystemError() : String("filesystem not mounted");
    return s_lastLoadStatus;
  }

  // Recover an interrupted atomic save (§15): a leftover temp or backup file means
  // writeConfigFile() was interrupted in the middle of its replacement sequence.
  // La recuperation elle-meme est dans ConfigPersist.cpp (pure, testee sur hote) :
  // si la configuration finale manque, elle promeut le .tmp (contenu le plus
  // recent, deja ecrit ET relu avant le remplacement), a defaut le .bak
  // (configuration precedente mise de cote) ; si elle est en place, les residus
  // sont perimes et sont effaces.
  const char* tmpPath = CONFIG_FILE_PATH ".tmp";
  const char* bakPath = CONFIG_FILE_PATH ".bak";
  if (LittleFS.exists(tmpPath) || LittleFS.exists(bakPath)) {
    // Sans residu il n'y a rien a recuperer : le demarrage nominal ne paie pas
    // l'appel (l'etat "final present, aucun residu" est exactement son no-op).
    configRecoverOnBoot(littleFsRenameOps(), tmpPath, CONFIG_FILE_PATH, bakPath);
  }

  File file = LittleFS.open(CONFIG_FILE_PATH, "r");
  if (!file) {
    if (DEBUG) {
      Serial.println("DEBUG: ConfigStorage - Pas de config sauvegardee, utilisation des defauts");
    }
    s_lastLoadStatus = CONFIG_DEFAULTS;
    return s_lastLoadStatus;
  }

  RuntimeConfig defaults = cfg;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, file);
  file.close();

  if (err) {
    if (DEBUG) {
      Serial.print("ERREUR: ConfigStorage - JSON invalide: ");
      Serial.println(err.c_str());
    }
    s_lastLoadStatus = CONFIG_STORAGE_ERROR;
    s_lastLoadError = String("JSON invalide: ") + err.c_str();
    cfg = defaults;
    return s_lastLoadStatus;
  }

  // --- Instrument ---
  cfg.numFingers = doc["num_fingers"] | cfg.numFingers;
  if (cfg.numFingers < 1) cfg.numFingers = 1;
  if (cfg.numFingers > MAX_FINGER_SERVOS) cfg.numFingers = MAX_FINGER_SERVOS;

  cfg.numNotes = doc["num_notes"] | cfg.numNotes;
  if (cfg.numNotes < 1) cfg.numNotes = 1;
  if (cfg.numNotes > MAX_NOTES) cfg.numNotes = MAX_NOTES;

  cfg.airflowPcaChannel = doc["air_pca"] | cfg.airflowPcaChannel;
  cfg.fingerAngleOpen = doc["angle_open"] | cfg.fingerAngleOpen;
  cfg.halfHolePercent = doc["half_hole_pct"] | cfg.halfHolePercent;
  if (doc.containsKey("embouchure")) {
    strncpy(cfg.embouchure, doc["embouchure"] | "trav", sizeof(cfg.embouchure) - 1);
    cfg.embouchure[sizeof(cfg.embouchure) - 1] = '\0';
  }

  // --- Fingers ---
  JsonArray fingers = doc["fingers"];
  if (fingers) {
    for (int i = 0; i < cfg.numFingers && i < (int)fingers.size(); i++) {
      JsonObject f = fingers[i];
      cfg.fingers[i].pcaChannel = f["ch"] | cfg.fingers[i].pcaChannel;
      cfg.fingers[i].closedAngle = f["a"] | cfg.fingers[i].closedAngle;
      cfg.fingers[i].direction = f["d"] | cfg.fingers[i].direction;
      cfg.fingers[i].isThumbHole = f["th"] | (cfg.fingers[i].isThumbHole ? 1 : 0);
      cfg.fingers[i].halfPercent = f["hp"] | cfg.fingers[i].halfPercent;
    }
  }

  // --- Notes (complete: MIDI + finger patterns + airflow) ---
  JsonArray notes = doc["notes"];
  if (notes) {
    int count = min((int)notes.size(), (int)MAX_NOTES);
    cfg.numNotes = count;
    for (int i = 0; i < count; i++) {
      JsonObject n = notes[i];
      int midiValue = n["midi"] | cfg.notes[i].midiNote;
      if (midiValue < 0 || midiValue > 127) {
        cfg.notes[i].midiNote = 255;  // sentinel rejected by validator before midiSeen indexing
      } else {
        cfg.notes[i].midiNote = (uint8_t)midiValue;
      }
      cfg.notes[i].airflowMinPercent = n["amn"] | cfg.notes[i].airflowMinPercent;
      cfg.notes[i].airflowMaxPercent = n["amx"] | cfg.notes[i].airflowMaxPercent;
      // Backward-compatible migration: if "anm" (nominal) is absent in the JSON,
      // derive it from min/max so old configs keep working.
      if (n.containsKey("anm")) {
        cfg.notes[i].airflowNominalPercent = n["anm"] | cfg.notes[i].airflowNominalPercent;
      } else {
        uint8_t mn = cfg.notes[i].airflowMinPercent;
        uint8_t mx = cfg.notes[i].airflowMaxPercent;
        cfg.notes[i].airflowNominalPercent =
            (mx >= mn) ? (uint8_t)(mn + (2 * (mx - mn)) / 5) : mn;  // min + 0.40*(max-min)
      }
      cfg.notes[i].anglePercent = n["ang"] | cfg.notes[i].anglePercent;
      JsonArray fp = n["fp"];
      if (fp) {
        for (int f = 0; f < MAX_FINGER_SERVOS; f++) {
          cfg.notes[i].fingerPattern[f] = (f < (int)fp.size()) ? (uint8_t)fp[f].as<int>() : 0;
        }
      }
    }
  }

  // --- Scalaires (surcharge partielle, chaque champ optionnel) ---
  cfg.midiChannel = doc["midi_ch"] | cfg.midiChannel;
  cfg.serialMidiEnabled = doc["smidi_on"] | (cfg.serialMidiEnabled ? 1 : 0);
  cfg.serialMidiRxPin = doc["smidi_rx"] | cfg.serialMidiRxPin;
  cfg.servoToSolenoidDelayMs = doc["servo_delay"] | cfg.servoToSolenoidDelayMs;
  cfg.minNoteIntervalForValveCloseMs = doc["valve_interval"] | cfg.minNoteIntervalForValveCloseMs;
  cfg.minNoteDurationMs = doc["min_note_dur"] | cfg.minNoteDurationMs;
  cfg.servoAirflowOff = doc["air_off"] | cfg.servoAirflowOff;
  cfg.servoAirflowMin = doc["air_min"] | cfg.servoAirflowMin;
  cfg.servoAirflowMax = doc["air_max"] | cfg.servoAirflowMax;
  if (doc.containsKey("ang_pca")) cfg.angleServoPcaChannel = doc["ang_pca"] | cfg.angleServoPcaChannel;
  if (doc.containsKey("angle_ch")) cfg.angleServoPcaChannel = doc["angle_ch"] | cfg.angleServoPcaChannel;  // canonical field wins over legacy ang_pca
  cfg.servoAngleOff = doc["ang_off"] | cfg.servoAngleOff;
  cfg.servoAngleMin = doc["ang_min"] | cfg.servoAngleMin;
  cfg.servoAngleMax = doc["ang_max"] | cfg.servoAngleMax;
  cfg.vibratoFrequencyHz = doc["vib_freq"] | cfg.vibratoFrequencyHz;
  cfg.vibratoMaxAmplitudeDeg = doc["vib_amp"] | cfg.vibratoMaxAmplitudeDeg;
  cfg.ccVolumeDefault = doc["cc_vol"] | cfg.ccVolumeDefault;
  cfg.ccExpressionDefault = doc["cc_expr"] | cfg.ccExpressionDefault;
  cfg.ccModulationDefault = doc["cc_mod"] | cfg.ccModulationDefault;
  cfg.ccBreathDefault = doc["cc_breath"] | cfg.ccBreathDefault;
  cfg.ccBrightnessDefault = doc["cc_bright"] | cfg.ccBrightnessDefault;
  cfg.cc2Enabled = doc["cc2_on"] | (cfg.cc2Enabled ? 1 : 0);
  cfg.cc2SilenceThreshold = doc["cc2_thr"] | cfg.cc2SilenceThreshold;
  cfg.cc2ResponseCurve = doc["cc2_curve"] | cfg.cc2ResponseCurve;
  cfg.cc2TimeoutMs = doc["cc2_timeout"] | cfg.cc2TimeoutMs;
  cfg.solenoidPwmActivation = doc["sol_act"] | cfg.solenoidPwmActivation;
  cfg.solenoidPwmHolding = doc["sol_hold"] | cfg.solenoidPwmHolding;
  cfg.solenoidActivationTimeMs = doc["sol_time"] | cfg.solenoidActivationTimeMs;
  cfg.timeUnpower = doc["time_unpower"] | cfg.timeUnpower;
  cfg.hideCalibration = doc["hide_calib"] | (cfg.hideCalibration ? 1 : 0);
  cfg.hideAir = doc["hide_air"] | (cfg.hideAir ? 1 : 0);
  cfg.solenoidPin = doc["sol_pin"] | cfg.solenoidPin;
  cfg.kbdMode = doc["kbd_mode"] | cfg.kbdMode;
  const char* color = doc["color"];
  if (color) { strncpy(cfg.instrumentColor, color, sizeof(cfg.instrumentColor) - 1); cfg.instrumentColor[sizeof(cfg.instrumentColor) - 1] = '\0'; }
  cfg.airAttackMode = doc["air_atk_mode"] | cfg.airAttackMode;
  cfg.airAttackOffset = doc["air_atk_off"] | cfg.airAttackOffset;
  cfg.airAttackMs = doc["air_atk_ms"] | cfg.airAttackMs;
  cfg.airVelocityResponse = doc["air_vel_resp"] | cfg.airVelocityResponse;

  // --- Air delivery system (modulaire) ---
  cfg.airMode = doc["air_mode"] | cfg.airMode;
  // Retro-compat: ancien mode 6 (endstop) -> mode 5 avec sensorType=endstop meca
  if (cfg.airMode == 6) {
    cfg.airMode = AIR_MODE_PUMP_RESERVOIR;
    if (!doc.containsKey("sens_type")) cfg.sensorType = SENSOR_TYPE_ENDSTOP_MECH;
  }
  cfg.valveType = doc["valve_type"] | cfg.valveType;
  // Retro-compat: ancien champ valve_servo (bool) -> valveType
  if (doc.containsKey("valve_servo") && !doc.containsKey("valve_type")) {
    cfg.valveType = doc["valve_servo"].as<bool>() ? 1 : 0;
  }
  cfg.valveServoPcaChannel = doc["valve_ch"] | cfg.valveServoPcaChannel;
  cfg.valveServoCloseAngle = doc["vlv_close"] | cfg.valveServoCloseAngle;
  cfg.valveServoOpenAngle = doc["vlv_open"] | cfg.valveServoOpenAngle;
  // vlv_dir is legacy and intentionally ignored; close/open angles define movement.
  // Compatibilite ascendante : ancienne cle "sol_inter".
  if (!doc.containsKey("valve_interval") && doc.containsKey("sol_inter")) cfg.minNoteIntervalForValveCloseMs = doc["sol_inter"] | cfg.minNoteIntervalForValveCloseMs;
  cfg.motorType = doc["motor_type"] | cfg.motorType;
  cfg.fanPin = doc["fan_pin"] | cfg.fanPin;
  cfg.fanMinPwm = doc["fan_min"] | cfg.fanMinPwm;
  cfg.fanMaxPwm = doc["fan_max"] | cfg.fanMaxPwm;
  cfg.fanIdlePercent = doc["fan_idle_pct"] | cfg.fanIdlePercent;
  cfg.fanIdleTimeoutMs = doc["fan_idle_timeout"] | cfg.fanIdleTimeoutMs;
  cfg.fanDefaultPercent = doc["fan_default_pct"] | cfg.fanDefaultPercent;
  cfg.fanMaxNotePercent = doc["fan_note_max_pct"] | cfg.fanMaxNotePercent;
  cfg.fanFollowAirflow = doc["fan_follow_air"] | (cfg.fanFollowAirflow ? 1 : 0);
  cfg.numPumps = doc["num_pumps"] | cfg.numPumps;
  if (cfg.numPumps < 1) cfg.numPumps = 1;
  if (cfg.numPumps > MAX_PUMPS) cfg.numPumps = MAX_PUMPS;
  // Retro-compat: ancien champ pump_pin unique -> pumpPins[0]
  if (doc.containsKey("pump_pin") && !doc.containsKey("pump_pins")) {
    cfg.pumpPins[0] = doc["pump_pin"] | cfg.pumpPins[0];
    cfg.pumpMinPwm[0] = doc["pump_min"] | cfg.pumpMinPwm[0];
    cfg.pumpMaxPwm[0] = doc["pump_max"] | cfg.pumpMaxPwm[0];
  }
  JsonArray pumpPins = doc["pump_pins"];
  if (pumpPins) {
    for (int i = 0; i < MAX_PUMPS && i < (int)pumpPins.size(); i++) {
      cfg.pumpPins[i] = pumpPins[i] | cfg.pumpPins[i];
    }
  }
  JsonArray pumpMins = doc["pump_mins"];
  if (pumpMins) {
    for (int i = 0; i < MAX_PUMPS && i < (int)pumpMins.size(); i++) {
      cfg.pumpMinPwm[i] = pumpMins[i] | cfg.pumpMinPwm[i];
    }
  }
  JsonArray pumpMaxs = doc["pump_maxs"];
  if (pumpMaxs) {
    for (int i = 0; i < MAX_PUMPS && i < (int)pumpMaxs.size(); i++) {
      cfg.pumpMaxPwm[i] = pumpMaxs[i] | cfg.pumpMaxPwm[i];
    }
  }
  cfg.pumpCascadeThreshold = doc["pump_cascade"] | cfg.pumpCascadeThreshold;
  if (cfg.pumpCascadeThreshold > 100) cfg.pumpCascadeThreshold = 100;
  cfg.pumpStaggerMs = doc["pump_stagger"] | cfg.pumpStaggerMs;
  cfg.pumpDirectIdlePercent = doc["pump_idle_pct"] | cfg.pumpDirectIdlePercent;
  cfg.pumpDirectMaxPercent = doc["pump_direct_max_pct"] | cfg.pumpDirectMaxPercent;
  cfg.pumpFollowAirflow = doc["pump_follow_air"] | (cfg.pumpFollowAirflow ? 1 : 0);
  cfg.reservoirTargetPercent = doc["res_target_pct"] | cfg.reservoirTargetPercent;
  cfg.reservoirAutoStart = doc["res_autostart"] | (cfg.reservoirAutoStart ? 1 : 0);
  cfg.bangbangHysteresis = doc["bb_hyst"] | cfg.bangbangHysteresis;
  if (cfg.bangbangHysteresis > 50) cfg.bangbangHysteresis = 50;
  cfg.sensorType = doc["sens_type"] | cfg.sensorType;
  cfg.sensorTargetMm = doc["sens_target"] | cfg.sensorTargetMm;
  cfg.sensorMinMm = doc["sens_min"] | cfg.sensorMinMm;
  cfg.sensorMaxMm = doc["sens_max"] | cfg.sensorMaxMm;
  cfg.pidKp = doc["pid_kp"] | cfg.pidKp;
  cfg.pidKi = doc["pid_ki"] | cfg.pidKi;
  cfg.endstopPin = doc["endstop_pin"] | cfg.endstopPin;
  cfg.endstopActiveHigh = doc["endstop_high"] | (cfg.endstopActiveHigh ? 1 : 0);
  cfg.endstopPumpOn = doc["endstop_pump_on"] | (cfg.endstopPumpOn ? 1 : 0);
  cfg.hallPin = doc["hall_pin"] | cfg.hallPin;
  cfg.hallThresholdLow = doc["hall_low"] | cfg.hallThresholdLow;
  cfg.hallThresholdHigh = doc["hall_high"] | cfg.hallThresholdHigh;
  cfg.angleServoEnabled = doc["angle_on"] | (cfg.angleServoEnabled ? 1 : 0);
  cfg.angleServoPcaChannel = doc["angle_ch"] | cfg.angleServoPcaChannel;
  cfg.showAirSystem = doc["show_air"] | (cfg.showAirSystem ? 1 : 0);
  const char* rf = doc["res_format"];
  if (rf) { strlcpy(cfg.resFormat, rf, sizeof(cfg.resFormat)); }

  // --- MIDI Storage ---
  cfg.midiStorageLimitKb = doc["midi_limit"] | cfg.midiStorageLimitKb;

  const char* ssid = doc["wifi_ssid"];
  if (ssid) { strncpy(cfg.wifiSsid, ssid, sizeof(cfg.wifiSsid) - 1); cfg.wifiSsid[sizeof(cfg.wifiSsid) - 1] = '\0'; }

  const char* pass = doc["wifi_pass"];
  if (pass) { strncpy(cfg.wifiPassword, pass, sizeof(cfg.wifiPassword) - 1); cfg.wifiPassword[sizeof(cfg.wifiPassword) - 1] = '\0'; }

  const char* name = doc["device"];
  if (name) { strncpy(cfg.deviceName, name, sizeof(cfg.deviceName) - 1); cfg.deviceName[sizeof(cfg.deviceName) - 1] = '\0'; }

  ConfigValidationResult validation = validateAndNormalizeConfig(cfg);
  if (!validation.valid) {
    if (DEBUG) {
      Serial.print("ERREUR: ConfigStorage - Config invalide, retour aux defauts: ");
      Serial.println(validation.error);
    }
    s_lastLoadStatus = CONFIG_INVALID_FALLBACK;
    s_lastLoadError = validation.error;
    cfg = defaults;
    validateAndNormalizeConfig(cfg);
    return s_lastLoadStatus;
  }
  s_lastLoadStatus = CONFIG_LOADED;

  if (DEBUG) {
    Serial.println("DEBUG: ConfigStorage - Config chargee depuis LittleFS");
    Serial.print("DEBUG:   Doigts: ");
    Serial.print(cfg.numFingers);
    Serial.print("  Notes: ");
    Serial.print(cfg.numNotes);
    Serial.print("  Airflow PCA: ");
    Serial.println(cfg.airflowPcaChannel);
  }

  return s_lastLoadStatus;
}

// Serialisation atomique du candidat DEJA valide (definie apres saveFrom, qui est
// son seul appelant : la validation precede toujours l'ecriture).
static bool writeConfigFile(const RuntimeConfig& source);

// Persiste la configuration DONNEE (et non forcement la configuration active).
// Le commit transactionnel de POST /api/config ecrit ainsi le candidat AVANT de
// le rendre actif : si l'ecriture echoue, rien n'a bouge ni en RAM ni en flash.
bool ConfigStorage::saveFrom(const RuntimeConfig& source) {
  // Jamais d'ecriture sur un systeme de fichiers non monte : l'appelant doit voir
  // un echec franc (et donc annuler sa transaction) plutot qu'une ecriture perdue.
  if (!isFilesystemMounted()) return false;

  // La validation porte sur SOURCE, jamais sur la configuration active.
  //
  // Elle portait sur `cfg`. Deux consequences, toutes deux reproduites :
  //  - un /config.json semantiquement invalide (deux doigts sur le meme canal PCA)
  //    laisse `cfg` invalide en RAM apres le passage en recovery. L'utilisateur
  //    corrigeait alors la configuration depuis l'interface web, le candidat etait
  //    bien valide par ConfigCommit... et saveFrom revalidait l'ANCIENNE `cfg`
  //    toujours invalide, echouait, et rendait storage_failed. Une configuration
  //    cassee n'etait donc plus reparable depuis le seul outil disponible ;
  //  - validateAndNormalizeConfig ECRIT dans son argument : `cfg` etait modifie
  //    hors du verrou de configuration pendant que la tache web peut le lire.
  // La copie de travail est sur le TAS : RuntimeConfig fait ~5 Ko et cette
  // fonction construit deja un JsonDocument de plusieurs kilo-octets ; 5 Ko de
  // plus sur la pile d'une tache Arduino/async suffisent a la faire deborder.
  RuntimeConfig* candidate = new (std::nothrow) RuntimeConfig();
  if (candidate == nullptr) {
    // Pas de memoire pour verifier : on refuse d'ecrire plutot que d'ecrire sans
    // avoir verifie. Un /config.json non valide condamne le prochain demarrage.
    if (DEBUG) { Serial.println("ERREUR: ConfigStorage - memoire insuffisante pour valider avant sauvegarde"); }
    return false;
  }
  ConfigValidationResult validation = validateCandidateConfig(source, *candidate);
  if (!validation.valid) {
    if (DEBUG) { Serial.print("ERREUR: ConfigStorage - sauvegarde refusee: "); Serial.println(validation.error); }
    delete candidate;
    return false;
  }
  // C'est le candidat NORMALISE qui part en flash : ce que le prochain demarrage
  // relira est exactement ce qui vient d'etre valide, sans derive.
  bool ok = writeConfigFile(*candidate);
  delete candidate;
  return ok;
}

static bool writeConfigFile(const RuntimeConfig& source) {
  JsonDocument doc;

  // --- Instrument ---
  doc["num_fingers"] = source.numFingers;
  doc["num_notes"] = source.numNotes;
  doc["air_pca"] = source.airflowPcaChannel;
  doc["angle_open"] = source.fingerAngleOpen;
  doc["half_hole_pct"] = source.halfHolePercent;
  doc["embouchure"] = source.embouchure;

  // --- Fingers ---
  JsonArray fingers = doc["fingers"].to<JsonArray>();
  for (int i = 0; i < source.numFingers; i++) {
    JsonObject f = fingers.add<JsonObject>();
    f["ch"] = source.fingers[i].pcaChannel;
    f["a"] = source.fingers[i].closedAngle;
    f["d"] = source.fingers[i].direction;
    if (source.fingers[i].isThumbHole) {
      f["th"] = 1;
    }
    if (source.fingers[i].halfPercent > 0) {
      f["hp"] = source.fingers[i].halfPercent;
    }
  }

  // --- Notes (complete) ---
  JsonArray notes = doc["notes"].to<JsonArray>();
  for (int i = 0; i < source.numNotes; i++) {
    JsonObject n = notes.add<JsonObject>();
    n["midi"] = source.notes[i].midiNote;
    n["amn"] = source.notes[i].airflowMinPercent;
    n["amx"] = source.notes[i].airflowMaxPercent;
    n["anm"] = source.notes[i].airflowNominalPercent;
    n["ang"] = source.notes[i].anglePercent;
    JsonArray fp = n["fp"].to<JsonArray>();
    for (int f = 0; f < MAX_FINGER_SERVOS; f++) {
      fp.add((int)source.notes[i].fingerPattern[f]);
    }
  }

  // --- Scalaires ---
  doc["midi_ch"] = source.midiChannel;
  doc["smidi_on"] = source.serialMidiEnabled ? 1 : 0;
  doc["smidi_rx"] = source.serialMidiRxPin;
  doc["servo_delay"] = source.servoToSolenoidDelayMs;
  doc["valve_interval"] = source.minNoteIntervalForValveCloseMs;
  doc["min_note_dur"] = source.minNoteDurationMs;
  doc["air_off"] = source.servoAirflowOff;
  doc["air_min"] = source.servoAirflowMin;
  doc["air_max"] = source.servoAirflowMax;
  doc["angle_ch"] = source.angleServoPcaChannel;
  doc["ang_off"] = source.servoAngleOff;
  doc["ang_min"] = source.servoAngleMin;
  doc["ang_max"] = source.servoAngleMax;
  doc["vib_freq"] = source.vibratoFrequencyHz;
  doc["vib_amp"] = source.vibratoMaxAmplitudeDeg;
  doc["cc_vol"] = source.ccVolumeDefault;
  doc["cc_expr"] = source.ccExpressionDefault;
  doc["cc_mod"] = source.ccModulationDefault;
  doc["cc_breath"] = source.ccBreathDefault;
  doc["cc_bright"] = source.ccBrightnessDefault;
  doc["cc2_on"] = source.cc2Enabled ? 1 : 0;
  doc["cc2_thr"] = source.cc2SilenceThreshold;
  doc["cc2_curve"] = source.cc2ResponseCurve;
  doc["cc2_timeout"] = source.cc2TimeoutMs;
  doc["sol_act"] = source.solenoidPwmActivation;
  doc["sol_hold"] = source.solenoidPwmHolding;
  doc["sol_time"] = source.solenoidActivationTimeMs;
  doc["time_unpower"] = source.timeUnpower;
  doc["hide_calib"] = source.hideCalibration ? 1 : 0;
  doc["hide_air"] = source.hideAir ? 1 : 0;
  doc["sol_pin"] = source.solenoidPin;
  doc["kbd_mode"] = source.kbdMode;
  doc["color"] = source.instrumentColor;
  doc["air_atk_mode"] = source.airAttackMode;
  doc["air_atk_off"] = source.airAttackOffset;
  doc["air_atk_ms"] = source.airAttackMs;
  doc["air_vel_resp"] = source.airVelocityResponse;
  // --- Air delivery system (modulaire) ---
  doc["air_mode"] = source.airMode;
  doc["valve_type"] = source.valveType;
  doc["valve_ch"] = source.valveServoPcaChannel;
  doc["vlv_close"] = source.valveServoCloseAngle;
  doc["vlv_open"] = source.valveServoOpenAngle;
  doc["motor_type"] = source.motorType;
  doc["fan_pin"] = source.fanPin;
  doc["fan_min"] = source.fanMinPwm;
  doc["fan_max"] = source.fanMaxPwm;
  doc["fan_idle_pct"] = source.fanIdlePercent;
  doc["fan_idle_timeout"] = source.fanIdleTimeoutMs;
  doc["fan_default_pct"] = source.fanDefaultPercent;
  doc["fan_note_max_pct"] = source.fanMaxNotePercent;
  doc["fan_follow_air"] = source.fanFollowAirflow ? 1 : 0;
  doc["num_pumps"] = source.numPumps;
  JsonArray pumpPins = doc["pump_pins"].to<JsonArray>();
  JsonArray pumpMins = doc["pump_mins"].to<JsonArray>();
  JsonArray pumpMaxs = doc["pump_maxs"].to<JsonArray>();
  for (int i = 0; i < MAX_PUMPS; i++) {
    pumpPins.add(source.pumpPins[i]);
    pumpMins.add(source.pumpMinPwm[i]);
    pumpMaxs.add(source.pumpMaxPwm[i]);
  }
  doc["pump_cascade"] = source.pumpCascadeThreshold;
  doc["pump_stagger"] = source.pumpStaggerMs;
  doc["pump_idle_pct"] = source.pumpDirectIdlePercent;
  doc["pump_direct_max_pct"] = source.pumpDirectMaxPercent;
  doc["pump_follow_air"] = source.pumpFollowAirflow ? 1 : 0;
  doc["res_target_pct"] = source.reservoirTargetPercent;
  doc["res_autostart"] = source.reservoirAutoStart ? 1 : 0;
  doc["bb_hyst"] = source.bangbangHysteresis;
  doc["sens_type"] = source.sensorType;
  doc["sens_target"] = source.sensorTargetMm;
  doc["sens_min"] = source.sensorMinMm;
  doc["sens_max"] = source.sensorMaxMm;
  doc["pid_kp"] = source.pidKp;
  doc["pid_ki"] = source.pidKi;
  doc["endstop_pin"] = source.endstopPin;
  doc["endstop_high"] = source.endstopActiveHigh ? 1 : 0;
  doc["endstop_pump_on"] = source.endstopPumpOn ? 1 : 0;
  doc["hall_pin"] = source.hallPin;
  doc["hall_low"] = source.hallThresholdLow;
  doc["hall_high"] = source.hallThresholdHigh;
  doc["angle_on"] = source.angleServoEnabled ? 1 : 0;
  doc["angle_ch"] = source.angleServoPcaChannel;
  doc["show_air"] = source.showAirSystem ? 1 : 0;
  doc["res_format"] = source.resFormat;
  doc["midi_limit"] = source.midiStorageLimitKb;
  doc["wifi_ssid"] = source.wifiSsid;
  doc["wifi_pass"] = source.wifiPassword;
  doc["device"] = source.deviceName;

  // Atomic write (§15): serialise into a temp file, verify it re-parses, then
  // replace the live config. Truncating the real file directly would destroy a
  // previously valid configuration if the write were interrupted or incomplete.
  const char* tmpPath = CONFIG_FILE_PATH ".tmp";
  File file = LittleFS.open(tmpPath, "w");
  if (!file) {
    if (DEBUG) {
      Serial.println("ERREUR: ConfigStorage - Impossible d'ecrire le fichier config temporaire");
    }
    return false;
  }

  size_t written = serializeJson(doc, file);
  file.close();

  if (written == 0) {
    LittleFS.remove(tmpPath);
    if (DEBUG) { Serial.println("ERREUR: ConfigStorage - Ecriture config vide"); }
    return false;
  }

  // Re-read the temp file and confirm it parses (and carries the expected shape)
  // before it is allowed to replace the good config.
  bool tmpOk = false;
  File verify = LittleFS.open(tmpPath, "r");
  if (verify) {
    JsonDocument check;
    DeserializationError verr = deserializeJson(check, verify);
    tmpOk = (!verr) && check["num_notes"].is<int>();
    verify.close();
  }
  if (!tmpOk) {
    LittleFS.remove(tmpPath);
    if (DEBUG) { Serial.println("ERREUR: ConfigStorage - Config temporaire invalide, ancienne conservee"); }
    return false;
  }

  // Replace the live config with the validated temp file, en trois temps (voir
  // ConfigPersist.h) : la configuration courante est DEPLACEE vers un .bak, jamais
  // supprimee, et ce .bak n'est efface qu'une fois la nouvelle en place.
  //
  // La sequence precedente etait "remove(config) ; si le rename echoue,
  // remove(tmp)" : un rename rate detruisait les DEUX copies (la bonne venait
  // d'etre supprimee, la seule valide restante l'etait juste apres), et la
  // recuperation au demarrage n'avait plus rien a promouvoir. Desormais un echec
  // laisse toujours soit la configuration restauree, soit un .tmp/.bak que
  // loadWithStatus() remet en place au demarrage suivant.
  const char* bakPath = CONFIG_FILE_PATH ".bak";
  if (!configAtomicReplace(littleFsRenameOps(), tmpPath, CONFIG_FILE_PATH, bakPath)) {
    if (DEBUG) { Serial.println("ERREUR: ConfigStorage - Remplacement atomique de la config echoue (ancienne conservee)"); }
    return false;
  }

  if (DEBUG) {
    Serial.print("DEBUG: ConfigStorage - Config sauvegardee (");
    Serial.print(written);
    Serial.println(" octets)");
  }

  return true;
}


bool ConfigStorage::save() {
  return saveFrom(cfg);
}
bool ConfigStorage::resetToDefaults() {
  // NE TOUCHE PAS `cfg`. La configuration active decrit le materiel REELLEMENT
  // initialise - canaux PCA, broches GPIO, angles fermes, sens de rotation. La
  // remplacer en RAM pendant que loop() tourne ferait piloter les actionneurs avec
  // une description qui ne correspond plus au montage cable (c'est exactement le
  // danger que bootConfigMayDriveActuators() decrit dans ConfigStorage.h).
  // On persiste donc un CANDIDAT ; l'activation se fait au REDEMARRAGE, que
  // l'appelant programme quand cette fonction rend true.
  if (!isFilesystemMounted()) return false;

  // Sur le TAS, pour la raison deja donnee dans saveFrom() : RuntimeConfig fait
  // ~5 Ko et cet appel vient d'une tache (web/async) a pile courte.
  RuntimeConfig* candidate = new (std::nothrow) RuntimeConfig();
  if (candidate == nullptr) {
    if (DEBUG) { Serial.println("ERREUR: ConfigStorage - memoire insuffisante pour preparer les defauts"); }
    return false;
  }
  makeDefaultConfig(*candidate);
  bool ok = saveFrom(*candidate);
  delete candidate;

  if (DEBUG) {
    if (ok) {
      Serial.println("DEBUG: ConfigStorage - Defauts persistes ; actifs au redemarrage (config active inchangee)");
    } else {
      Serial.println("ERREUR: ConfigStorage - Persistance des defauts echouee ; config active et fichier inchanges");
    }
  }
  return ok;
}

bool ConfigStorage::factoryReset() {
  if (!isFilesystemMounted()) return false;
  // NE TOUCHE PAS `cfg` non plus (meme raison que resetToDefaults) : le materiel
  // continue d'etre pilote par la configuration qui le decrit jusqu'au
  // redemarrage, apres quoi la machine repart en premier demarrage.
  // C'etait le pire des deux cas : factoryReset() detruisait la configuration
  // active en RAM sans rien persister, donc jusqu'au redemarrage le firmware
  // pilotait du materiel avec une configuration d'usine arbitraire.
  const char* tmpPath = CONFIG_FILE_PATH ".tmp";
  const char* bakPath = CONFIG_FILE_PATH ".bak";

  // Supprimer le fichier config pour que isFirstBoot() retourne true
  // Le wizard le recreera via save() apres configuration
  bool ok = LittleFS.remove(CONFIG_FILE_PATH) || !LittleFS.exists(CONFIG_FILE_PATH);

  // Les residus comptent autant que le fichier lui-meme : loadWithStatus() promeut
  // un .tmp ou un .bak quand la configuration finale manque. En laisser un
  // ressusciterait au demarrage suivant la configuration que l'utilisateur vient
  // d'effacer, alors que isFirstBoot() aurait annonce une machine vierge.
  if (LittleFS.exists(tmpPath)) LittleFS.remove(tmpPath);
  if (LittleFS.exists(bakPath)) LittleFS.remove(bakPath);
  ok = ok && !LittleFS.exists(tmpPath) && !LittleFS.exists(bakPath);

  if (DEBUG) {
    Serial.println("DEBUG: ConfigStorage - Reset usine (fichier + residus supprimes)");
  }
  return ok;
}

bool ConfigStorage::isFirstBoot() {
  if (!isFilesystemMounted()) return false;
  // Premier demarrage = AUCUNE configuration recuperable. Un .tmp ou un .bak
  // orphelin serait promu par loadWithStatus() au prochain demarrage : annoncer
  // une machine vierge alors que la configuration precedente va revenir ferait
  // ecrire l'assistant de premier demarrage par-dessus une config bien vivante.
  return !LittleFS.exists(CONFIG_FILE_PATH) &&
         !LittleFS.exists(CONFIG_FILE_PATH ".tmp") &&
         !LittleFS.exists(CONFIG_FILE_PATH ".bak");
}

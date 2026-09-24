/***********************************************************************************************
 * test_fin_storage - Finalisation de la persistance de configuration.
 *
 * Trois defauts, chacun REPRODUIT par du code execute avant d'etre corrige :
 *
 *  B-1 : la recuperation au demarrage pouvait appliquer une sauvegarde DECLAREE
 *        ECHOUEE. Quand configAtomicReplace() rendait false en laissant
 *        tmp = NEW et bak = OLD sans configuration finale, le demarrage suivant
 *        promouvait le .tmp : l'utilisateur avait vu une erreur d'enregistrement,
 *        et NEW devenait quand meme la configuration active. Une operation ratee
 *        devenait silencieusement une operation reussie, un reboot plus tard.
 *        Nouvel invariant : live absent + bak present => le BAK gagne.
 *
 *  B-2 : le reset usine n'etait pas transactionnel. Il supprimait /config.json
 *        EN PREMIER puis les residus, et rendait false si un residu resistait :
 *        l'utilisateur recevait une erreur ET avait quand meme perdu sa
 *        configuration. Nouvel ordre : residus d'abord, live en dernier, et
 *        jamais "live detruit + false".
 *
 *  B-3 : la table de topologie materielle. InstrumentManager listait les champs
 *        exigeant un redemarrage dans une longue disjonction, ou manquaient
 *        endstopPin, endstopActiveHigh et hallPin - trois champs sur lesquels
 *        PressureController::begin() fait pinMode(). La table de ConfigTopology
 *        se parcourt : les tests ci-dessous exigent qu'elle et eux se connaissent
 *        MUTUELLEMENT, donc un champ ajoute ou retire sans test echoue.
 *
 * Point d'entree : fin_storage_run_all_tests().
 ***********************************************************************************************/
void fin_storage_run_all_tests();

#include <cassert>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <string>

#include "Arduino.h"
#include "ConfigPersist.h"
#include "ConfigStorage.h"
#include "ConfigTopology.h"

namespace {

const char* const kLive = CONFIG_FILE_PATH;
const char* const kTmp = CONFIG_FILE_PATH ".tmp";
const char* const kBak = CONFIG_FILE_PATH ".bak";

// =============================================================================
// Faux systeme de fichiers
//
// Volontairement plus petit que celui de test_harden_storage.cpp : ces tests-ci
// n'ont besoin que de trois pannes - un remove cible qui refuse, un rename cible
// qui refuse, et un systeme de fichiers qui pretend que tout existe. Le reste
// (compteurs d'appels par operation, echec global des renames) n'aurait servi a
// rien ici, et une aide inutilisee est du code mort dans une suite de tests.
// =============================================================================

struct FinFs {
  std::map<std::string, std::string> files;
  std::string failRemove;                    // ce chemin refuse d'etre supprime
  std::string failRenameFrom, failRenameTo;  // ce rename precis refuse
  bool lieExistsAlwaysTrue;                  // systeme de fichiers qui acquitte tout

  FinFs() : lieExistsAlwaysTrue(false) {}

  bool has(const char* p) const { return files.count(p) != 0; }
  std::string get(const char* p) const {
    std::map<std::string, std::string>::const_iterator it = files.find(p);
    return it == files.end() ? std::string() : it->second;
  }
  void put(const char* p, const char* content) { files[p] = content; }
  size_t count() const { return files.size(); }
};

bool finExists(void* ctx, const char* path) {
  FinFs* fs = static_cast<FinFs*>(ctx);
  if (fs->lieExistsAlwaysTrue) return true;
  return fs->files.count(path) != 0;
}

bool finRemove(void* ctx, const char* path) {
  FinFs* fs = static_cast<FinFs*>(ctx);
  if (!fs->failRemove.empty() && fs->failRemove == path) return false;
  return fs->files.erase(path) > 0;
}

bool finRename(void* ctx, const char* from, const char* to) {
  FinFs* fs = static_cast<FinFs*>(ctx);
  if (!fs->failRenameFrom.empty() && fs->failRenameFrom == from && fs->failRenameTo == to) {
    return false;
  }
  std::map<std::string, std::string>::iterator it = fs->files.find(from);
  if (it == fs->files.end()) return false;
  fs->files[to] = it->second;
  fs->files.erase(it);
  return true;
}

FsRenameOps opsFor(FinFs& fs) {
  FsRenameOps ops;
  ops.exists = &finExists;
  ops.remove = &finRemove;
  ops.rename = &finRename;
  ops.ctx = &fs;
  return ops;
}

// =============================================================================
// B-1 - une sauvegarde annoncee ECHOUEE n'est jamais appliquee
// =============================================================================

// LE test de B-1. L'etat de depart est exactement celui que laisse un
// configAtomicReplace() qui a rendu false : la configuration courante a ete mise
// de cote en .bak, la promotion du .tmp a echoue, et la restauration du .bak a
// echoue aussi. L'utilisateur a vu "enregistrement impossible".
//
// SUR LE CODE D'AVANT : le .tmp etait promu en premier, donc fs[kLive] == "NEW"
// et l'assertion ci-dessous echouait avec "NEW" != "OLD".
void fin_storage_failed_save_is_never_applied_on_next_boot() {
  FinFs fs;
  fs.put(kBak, "OLD");   // configuration COMMITEE, mise de cote a l'etape 2
  fs.put(kTmp, "NEW");   // candidat dont la promotion a ECHOUE
  // pas de kLive : c'est tout le probleme

  assert(configRecoverOnBoot(opsFor(fs), kTmp, kLive, kBak));

  // La configuration remise en place est celle d'AVANT la tentative.
  assert(fs.get(kLive) == "OLD");
  // Et le candidat refuse ne traine plus : le laisser ferait revenir la question
  // au demarrage d'apres.
  assert(!fs.has(kTmp));
  assert(!fs.has(kBak));
  assert(fs.count() == 1);
}

// Le .tmp SEUL reste promu : il n'y a pas de .bak, donc aucune configuration n'a
// jamais ete commitee. C'est un tout premier enregistrement interrompu, et le
// .tmp est la seule chose qui existe - le refuser laisserait une machine vierge.
void fin_storage_lone_tmp_is_still_promoted() {
  FinFs fs;
  fs.put(kTmp, "FIRST");

  assert(configRecoverOnBoot(opsFor(fs), kTmp, kLive, kBak));
  assert(fs.get(kLive) == "FIRST");
  assert(fs.count() == 1);
}

// Configuration en place : on n'y touche pas, et les deux residus sont effaces.
// Comportement existant, verrouille ici parce que le nouvel ordre de promotion
// aurait pu l'emporter par inadvertance.
void fin_storage_live_config_wins_over_leftovers() {
  FinFs fs;
  fs.put(kLive, "LIVE");
  fs.put(kTmp, "CANDIDAT");
  fs.put(kBak, "ANCIEN");

  assert(configRecoverOnBoot(opsFor(fs), kTmp, kLive, kBak));
  assert(fs.get(kLive) == "LIVE");
  assert(!fs.has(kTmp));
  assert(!fs.has(kBak));
  assert(fs.count() == 1);
}

// L'INVARIANT UTILISATEUR, bout en bout : une sauvegarde qui rend false n'est
// jamais appliquee, meme apres redemarrage. Rien n'est fabrique a la main ici -
// c'est configAtomicReplace() qui produit l'etat, exactement comme en production.
void fin_storage_failed_replace_then_boot_returns_the_previous_config() {
  FinFs fs;
  fs.put(kLive, "OLD");
  fs.put(kTmp, "NEW");
  // La mise de cote (live -> bak) passe ; la promotion (tmp -> live) echoue, et
  // la restauration (bak -> live) aussi : c'est le meme rename, dans l'autre
  // sens, sur un systeme de fichiers qui refuse d'ecrire a cet emplacement.
  fs.failRenameFrom = kTmp;
  fs.failRenameTo = kLive;

  // 1. La sauvegarde est annoncee ECHOUEE a l'utilisateur.
  assert(!configAtomicReplace(opsFor(fs), kTmp, kLive, kBak));

  // La restauration bak -> live, elle, a fonctionne : la configuration courante
  // est intacte. Ce chemin-la etait deja bon ; on le constate pour que le cas
  // suivant soit bien le cas DIFFICILE et pas celui-ci.
  assert(fs.get(kLive) == "OLD");

  // 2. Le cas difficile : plus aucun rename vers kLive ne fonctionne, y compris
  //    la restauration. L'etat laisse est bak = OLD, tmp = NEW, pas de live.
  FinFs hard;
  hard.put(kLive, "OLD");
  hard.put(kTmp, "NEW");
  FsRenameOps hardOps = opsFor(hard);
  assert(hardOps.rename(hardOps.ctx, kLive, kBak));   // etape 2 jouee a la main
  hard.failRenameFrom = kTmp;
  hard.failRenameTo = kLive;
  assert(!configAtomicReplace(hardOps, kTmp, kLive, kBak));
  assert(!hard.has(kLive));
  assert(hard.get(kBak) == "OLD");
  assert(hard.get(kTmp) == "NEW");

  // 3. Le systeme de fichiers se remet ; l'instrument redemarre. La configuration
  //    lue est celle d'AVANT la tentative, pas celle que l'utilisateur a vu
  //    echouer.
  hard.failRenameFrom.clear();
  hard.failRenameTo.clear();
  assert(configRecoverOnBoot(opsFor(hard), kTmp, kLive, kBak));
  assert(hard.get(kLive) == "OLD");
  assert(hard.count() == 1);
}

// Le .bak est la mais refuse de bouger. On NE se rabat PAS sur le .tmp : mieux
// vaut demarrer sans configuration (mode recovery, interface web disponible) que
// piloter du materiel avec une configuration dont l'enregistrement a ete annonce
// echoue. Rien n'est efface, donc le demarrage suivant retentera.
void fin_storage_unmovable_backup_never_falls_back_to_the_candidate() {
  FinFs fs;
  fs.put(kBak, "OLD");
  fs.put(kTmp, "NEW");
  fs.failRenameFrom = kBak;
  fs.failRenameTo = kLive;

  assert(!configRecoverOnBoot(opsFor(fs), kTmp, kLive, kBak));
  assert(!fs.has(kLive));
  assert(fs.get(kBak) == "OLD");   // toujours recuperable
  assert(fs.get(kTmp) == "NEW");   // rien n'a ete detruit non plus
}

// Rien a recuperer : ni configuration, ni copie. Et surtout rien de cree.
void fin_storage_empty_filesystem_recovers_nothing() {
  FinFs fs;
  assert(!configRecoverOnBoot(opsFor(fs), kTmp, kLive, kBak));
  assert(fs.count() == 0);
}

// =============================================================================
// B-2 - le reset usine ne detruit jamais la configuration pour rendre false
// =============================================================================

// L'ORDRE D'ORIGINE de ConfigStorage::factoryReset(), conserve ici et seulement
// ici pour que le defaut reste demontre par du code execute :
//     bool ok = remove(live) || !exists(live);
//     if (exists(tmp)) remove(tmp);
//     if (exists(bak)) remove(bak);
//     ok = ok && !exists(tmp) && !exists(bak);
bool legacyFactoryErase(FinFs& fs) {
  bool ok = finRemove(&fs, kLive) || !finExists(&fs, kLive);
  if (finExists(&fs, kTmp)) finRemove(&fs, kTmp);
  if (finExists(&fs, kBak)) finRemove(&fs, kBak);
  return ok && !finExists(&fs, kTmp) && !finExists(&fs, kBak);
}

// Le defaut B-2, execute : la sequence d'origine rend false APRES avoir detruit
// la configuration. L'utilisateur recoit une erreur et a quand meme tout perdu.
void fin_storage_legacy_factory_reset_destroys_then_reports_failure() {
  FinFs fs;
  fs.put(kLive, "OLD");
  fs.put(kBak, "ANCIEN");
  fs.failRemove = kBak;   // un residu qui refuse de disparaitre

  assert(!legacyFactoryErase(fs));    // erreur annoncee...
  assert(!fs.has(kLive));             // ... et configuration DEJA detruite
  assert(fs.get(kBak) == "ANCIEN");   // pendant que le residu, lui, est reste
}

// Cas nominal : tout part, et la fonction le dit.
void fin_storage_factory_reset_nominal() {
  FinFs fs;
  fs.put(kLive, "OLD");
  fs.put(kTmp, "CANDIDAT");
  fs.put(kBak, "ANCIEN");

  assert(configFactoryErase(opsFor(fs), kTmp, kLive, kBak));
  assert(fs.count() == 0);

  // Idempotent : un systeme de fichiers deja vierge est un succes, pas une erreur.
  assert(configFactoryErase(opsFor(fs), kTmp, kLive, kBak));
  assert(fs.count() == 0);
}

// INJECTION DE PANNE A CHAQUE ETAPE. La regle, a chaque fois : ou bien l'etat
// final demande est atteint et on rend true, ou bien LA CONFIGURATION LIVE EST
// ENCORE LA et on rend false. Jamais "live detruit + false".
void fin_storage_factory_reset_failures_never_lose_the_config() {
  // a. Le .tmp refuse de disparaitre : abandon avant tout degat.
  {
    FinFs fs;
    fs.put(kLive, "OLD");
    fs.put(kTmp, "CANDIDAT");
    fs.put(kBak, "ANCIEN");
    fs.failRemove = kTmp;

    assert(!configFactoryErase(opsFor(fs), kTmp, kLive, kBak));
    assert(fs.get(kLive) == "OLD");   // INTACTE
    assert(fs.get(kTmp) == "CANDIDAT");
    // Le .bak n'a meme pas ete touche : on s'arrete au premier refus.
    assert(fs.get(kBak) == "ANCIEN");
  }

  // b. Le .bak refuse de disparaitre : meme conclusion. C'est exactement le
  //    scenario que la sequence d'origine transformait en perte de donnees
  //    (voir fin_storage_legacy_factory_reset_destroys_then_reports_failure).
  {
    FinFs fs;
    fs.put(kLive, "OLD");
    fs.put(kTmp, "CANDIDAT");
    fs.put(kBak, "ANCIEN");
    fs.failRemove = kBak;

    assert(!configFactoryErase(opsFor(fs), kTmp, kLive, kBak));
    assert(fs.get(kLive) == "OLD");   // INTACTE
    assert(fs.get(kBak) == "ANCIEN");
  }

  // c. La configuration live elle-meme refuse de disparaitre. On rend false, et
  //    elle est toujours la : l'instrument redemarre exactement comme avant.
  {
    FinFs fs;
    fs.put(kLive, "OLD");
    fs.put(kTmp, "CANDIDAT");
    fs.failRemove = kLive;

    assert(!configFactoryErase(opsFor(fs), kTmp, kLive, kBak));
    assert(fs.get(kLive) == "OLD");   // INTACTE
    assert(!fs.has(kTmp));            // le residu, lui, est bien parti

    // Et un demarrage ici retrouve cette configuration : l'utilisateur n'a rien
    // perdu du tout.
    assert(configRecoverOnBoot(opsFor(fs), kTmp, kLive, kBak));
    assert(fs.get(kLive) == "OLD");
  }

  // d. Systeme de fichiers qui acquitte tout sans rien faire : exists() ment,
  //    donc l'etat demande n'est jamais atteint. On rend false sans boucler.
  {
    FinFs fs;
    fs.put(kLive, "OLD");
    fs.lieExistsAlwaysTrue = true;

    assert(!configFactoryErase(opsFor(fs), kTmp, kLive, kBak));
    assert(fs.get(kLive) == "OLD");
  }

  // e. Jeu d'operations incomplet ou chemin nul : refus franc, aucune ecriture.
  {
    FinFs fs;
    fs.put(kLive, "OLD");
    FsRenameOps broken = opsFor(fs);
    broken.remove = nullptr;
    assert(!configFactoryErase(broken, kTmp, kLive, kBak));
    assert(fs.get(kLive) == "OLD");

    assert(!configFactoryErase(opsFor(fs), kTmp, nullptr, kBak));
    assert(fs.get(kLive) == "OLD");
  }

  // f. L'effacement n'utilise PAS rename() : un jeu d'operations sans rename
  //    doit reussir, pas echouer sur un pointeur dont il ne se sert pas.
  {
    FinFs fs;
    fs.put(kLive, "OLD");
    FsRenameOps noRename = opsFor(fs);
    noRename.rename = nullptr;
    assert(configFactoryErase(noRename, kTmp, kLive, kBak));
    assert(fs.count() == 0);
  }
}

// Apres un reset usine reussi, le demarrage suivant ne ressuscite rien : c'est
// la raison d'etre de l'effacement des residus.
void fin_storage_factory_reset_leaves_nothing_to_recover() {
  FinFs fs;
  fs.put(kLive, "OLD");
  fs.put(kTmp, "CANDIDAT");
  fs.put(kBak, "ANCIEN");

  assert(configFactoryErase(opsFor(fs), kTmp, kLive, kBak));
  assert(!configRecoverOnBoot(opsFor(fs), kTmp, kLive, kBak));
  assert(fs.count() == 0);
}

// =============================================================================
// B-3 - la table de topologie materielle
// =============================================================================

// strncpy(dst, s, sizeof(dst) - 1) sur une chaine qui remplit exactement le
// tampon declenche un avertissement de troncature, alors que le resultat est
// correct : ces tampons viennent d'un memset a zero, le terminateur est deja la.
// On copie explicitement plutot que de laisser un avertissement s'installer.
void setText(char* dst, size_t size, const char* value) {
  size_t n = strlen(value);
  if (n >= size) n = size - 1;
  memcpy(dst, value, n);
  dst[n] = '\0';
}

// Une configuration de reference, reconnaissable et a zero partout ailleurs :
// RuntimeConfig contient des tableaux et du bourrage, et chaque mutation
// ci-dessous doit etre le SEUL changement entre deux copies.
void topologyBaseline(RuntimeConfig& c) {
  memset(&c, 0, sizeof(c));
  c.numFingers = 3;
  c.numNotes = 5;
  c.fingers[0].pcaChannel = 4;
  c.fingers[1].pcaChannel = 5;
  c.fingers[2].pcaChannel = 6;
  c.fingers[0].closedAngle = 100;
  c.notes[0].midiNote = 62;
  c.notes[0].airflowNominalPercent = 50;
  c.airflowPcaChannel = 11;
  c.angleServoPcaChannel = 12;
  c.valveServoPcaChannel = 13;
  c.airMode = 5;                 // pompe + reservoir : tous les begin() concernes
  c.valveType = 0;
  c.motorType = 0;
  c.solenoidPin = 19;
  c.fanPin = 23;
  c.numPumps = 2;
  c.pumpPins[0] = 25;
  c.pumpPins[1] = 26;
  c.sensorType = 2;              // Hall
  c.endstopPin = 34;
  c.endstopActiveHigh = false;
  c.hallPin = 35;
  c.serialMidiEnabled = false;
  c.serialMidiRxPin = 16;
  c.ccVolumeDefault = 100;
  c.midiChannel = 1;
  c.pidKp = 10;
  c.sensorTargetMm = 120;
  c.hallThresholdHigh = 3000;
  c.vibratoFrequencyHz = 5.0f;
  c.servoAirflowMax = 150;
  setText(c.embouchure, sizeof(c.embouchure), "trav");
  setText(c.deviceName, sizeof(c.deviceName), "flute-atelier");
}

// Un cas = le nom d'une entree de la TABLE, et la mutation qui la declenche.
// Le nom est ce qui rend la couverture verifiable dans les deux sens.
struct TopoCase {
  const char* name;
  void (*mutate)(RuntimeConfig& c);
};

void mutNumFingers(RuntimeConfig& c) { c.numFingers = 5; }
void mutFingerPca(RuntimeConfig& c) { c.fingers[1].pcaChannel = 17; }
void mutAirflowPca(RuntimeConfig& c) { c.airflowPcaChannel = 18; }
void mutAngleServoEnabled(RuntimeConfig& c) { c.angleServoEnabled = true; }
void mutAngleServoPca(RuntimeConfig& c) { c.angleServoPcaChannel = 19; }
void mutAirMode(RuntimeConfig& c) { c.airMode = 4; }
void mutValveType(RuntimeConfig& c) { c.valveType = 1; }
void mutValveServoPca(RuntimeConfig& c) { c.valveServoPcaChannel = 20; }
void mutSolenoidPin(RuntimeConfig& c) { c.solenoidPin = 18; }
void mutFanPin(RuntimeConfig& c) { c.fanPin = 22; }
void mutNumPumps(RuntimeConfig& c) { c.numPumps = 3; }
void mutPumpPins(RuntimeConfig& c) { c.pumpPins[1] = 27; }
void mutMotorType(RuntimeConfig& c) { c.motorType = 1; }
void mutSensorType(RuntimeConfig& c) { c.sensorType = 3; }
void mutEndstopPin(RuntimeConfig& c) { c.endstopPin = 36; }
void mutEndstopActiveHigh(RuntimeConfig& c) { c.endstopActiveHigh = true; }
void mutHallPin(RuntimeConfig& c) { c.hallPin = 39; }
void mutSerialMidiEnabled(RuntimeConfig& c) { c.serialMidiEnabled = true; }
void mutSerialMidiRxPin(RuntimeConfig& c) { c.serialMidiRxPin = 17; }

// La liste vit DANS une fonction, pas au niveau du fichier. L'analyse
// d'atteignabilite de tests/test_suite_integrity.py ne parcourt que des CORPS de
// fonctions : un mutateur reference uniquement depuis une liste d'initialisation
// au niveau du fichier passerait pour du code mort, et ferait echouer le
// detecteur sur une trentaine de faux positifs.
size_t topologyCases(const TopoCase** out) {
  static const TopoCase kCases[] = {
    { "numFingers",           &mutNumFingers },
    { "fingers[].pcaChannel", &mutFingerPca },
    { "airflowPcaChannel",    &mutAirflowPca },
    { "angleServoEnabled",    &mutAngleServoEnabled },
    { "angleServoPcaChannel", &mutAngleServoPca },
    { "airMode",              &mutAirMode },
    { "valveType",            &mutValveType },
    { "valveServoPcaChannel", &mutValveServoPca },
    { "solenoidPin",          &mutSolenoidPin },
    { "fanPin",               &mutFanPin },
    { "numPumps",             &mutNumPumps },
    { "pumpPins[]",           &mutPumpPins },
    { "motorType",            &mutMotorType },
    { "sensorType",           &mutSensorType },
    { "endstopPin",           &mutEndstopPin },
    { "endstopActiveHigh",    &mutEndstopActiveHigh },
    { "hallPin",              &mutHallPin },
    { "serialMidiEnabled",    &mutSerialMidiEnabled },
    { "serialMidiRxPin",      &mutSerialMidiRxPin },
  };
  *out = kCases;
  return sizeof(kCases) / sizeof(kCases[0]);
}

// Une configuration identique a elle-meme n'exige evidemment rien - mais c'est
// le seul test qui empeche une table cassee de rendre true pour tout.
void fin_storage_topology_identical_configs_need_no_restart() {
  RuntimeConfig a, b;
  topologyBaseline(a);
  topologyBaseline(b);
  assert(memcmp(&a, &b, sizeof(RuntimeConfig)) == 0);
  assert(!configTopologyRequiresRestart(a, b));
  assert(configTopologyChangedField(a, b) == nullptr);
}

// LES TROIS CHAMPS MANQUANTS, nommes explicitement. Sur la disjonction de
// InstrumentManager::configChangeRequiresRestart(), les trois rendaient false
// alors que PressureController::begin() fait pinMode() sur chacun.
void fin_storage_topology_covers_the_three_missing_pins() {
  RuntimeConfig base;
  topologyBaseline(base);

  // endstopPin : PressureController.cpp pinMode(cfg.endstopPin, ...)
  {
    RuntimeConfig next = base;
    next.endstopPin = 36;
    assert(configTopologyRequiresRestart(base, next));
    assert(strcmp(configTopologyChangedField(base, next), "endstopPin") == 0);
  }

  // endstopActiveHigh : choisit INPUT_PULLUP ou INPUT_PULLDOWN. Se tromper de
  // rappel interne fait lire "capteur actif" en permanence, donc "reservoir
  // plein" en permanence - la pompe ne demarre jamais.
  {
    RuntimeConfig next = base;
    next.endstopActiveHigh = true;
    assert(configTopologyRequiresRestart(base, next));
    assert(strcmp(configTopologyChangedField(base, next), "endstopActiveHigh") == 0);
  }

  // hallPin : PressureController.cpp pinMode(cfg.hallPin, INPUT) + sonde ADC.
  {
    RuntimeConfig next = base;
    next.hallPin = 39;
    assert(configTopologyRequiresRestart(base, next));
    assert(strcmp(configTopologyChangedField(base, next), "hallPin") == 0);
  }
}

// Chaque entree de la table est SENSIBLE : la mutation qui lui correspond
// declenche un redemarrage, et c'est bien cette entree qui est nommee.
void fin_storage_topology_every_table_entry_is_reachable() {
  RuntimeConfig base;
  topologyBaseline(base);

  const TopoCase* cases = nullptr;
  const size_t n = topologyCases(&cases);
  for (size_t i = 0; i < n; i++) {
    RuntimeConfig next = base;
    cases[i].mutate(next);
    // La mutation change reellement quelque chose (sinon le cas serait vide).
    assert(memcmp(&base, &next, sizeof(RuntimeConfig)) != 0);
    assert(configTopologyRequiresRestart(base, next));
    const char* named = configTopologyChangedField(base, next);
    assert(named != nullptr);
    assert(strcmp(named, cases[i].name) == 0);
    // Symetrique : l'ordre des deux configurations ne change pas la conclusion.
    assert(configTopologyRequiresRestart(next, base));
  }
}

// LA COUVERTURE, dans les DEUX SENS. C'est ce test qui fait qu'un champ oublie
// se voit : ajouter une entree a la table sans ecrire le cas correspondant
// echoue ici, et retirer une entree de la table echoue ici aussi.
void fin_storage_topology_table_and_tests_know_each_other() {
  std::set<std::string> inTable;
  for (size_t i = 0; i < configTopologyFieldCount(); i++) {
    const ConfigTopologyField& f = configTopologyFieldAt(i);
    assert(f.name != nullptr && f.name[0] != '\0');
    assert(f.changed != nullptr);
    // La colonne "applique par" n'est pas decorative : c'est la seule trace du
    // begin() qui justifie la ligne. Une ligne sans justification est une ligne
    // que personne ne saura relire.
    assert(f.appliedBy != nullptr && f.appliedBy[0] != '\0');
    // Pas de doublon : deux lignes pour le meme champ masqueraient une erreur de
    // copier-coller dans les comparateurs.
    assert(inTable.insert(std::string(f.name)).second);
  }

  const TopoCase* cases = nullptr;
  const size_t n = topologyCases(&cases);
  std::set<std::string> inTests;
  for (size_t i = 0; i < n; i++) {
    inTests.insert(std::string(cases[i].name));
  }

  assert(inTable == inTests);
  assert(inTable.size() == configTopologyFieldCount());
  assert(inTests.size() == n);
  // Que la table ne s'etende pas jusqu'a couvrir toute la configuration est
  // verifie par le COMPORTEMENT, pas par un compte :
  // fin_storage_topology_musical_changes_never_reboot() exige qu'une trentaine de
  // champs musicaux et d'interface la traversent sans declencher de redemarrage.
}

// Un cas = un champ purement musical, d'interface ou de regulation, et la
// mutation qui le change.
struct MusicalCase {
  const char* name;
  void (*mutate)(RuntimeConfig& c);
};

void mutCcVolume(RuntimeConfig& c) { c.ccVolumeDefault = 42; }
void mutCcBreath(RuntimeConfig& c) { c.ccBreathDefault = 77; }
void mutNoteAirflow(RuntimeConfig& c) { c.notes[0].airflowNominalPercent = 80; }
void mutNumNotes(RuntimeConfig& c) { c.numNotes = 12; }
void mutClosedAngle(RuntimeConfig& c) { c.fingers[0].closedAngle = 140; }
void mutFingerDirection(RuntimeConfig& c) { c.fingers[0].direction = -1; }
void mutHalfHolePercent(RuntimeConfig& c) { c.halfHolePercent = 60; }
void mutServoAirflowMax(RuntimeConfig& c) { c.servoAirflowMax = 170; }
void mutVibratoHz(RuntimeConfig& c) { c.vibratoFrequencyHz = 6.5f; }
void mutAirAttackMs(RuntimeConfig& c) { c.airAttackMs = 250; }
void mutSolenoidPwmHolding(RuntimeConfig& c) { c.solenoidPwmHolding = 90; }
void mutFanMaxPwm(RuntimeConfig& c) { c.fanMaxPwm = 220; }
void mutPumpMaxPwm(RuntimeConfig& c) { c.pumpMaxPwm[0] = 200; }
void mutPumpStagger(RuntimeConfig& c) { c.pumpStaggerMs = 120; }
void mutPidKp(RuntimeConfig& c) { c.pidKp = 25; }
void mutSensorTargetMm(RuntimeConfig& c) { c.sensorTargetMm = 200; }
void mutHallThreshold(RuntimeConfig& c) { c.hallThresholdHigh = 3500; }
void mutEndstopPumpOn(RuntimeConfig& c) { c.endstopPumpOn = true; }
void mutReservoirTarget(RuntimeConfig& c) { c.reservoirTargetPercent = 70; }
void mutReservoirAutoStart(RuntimeConfig& c) { c.reservoirAutoStart = true; }
void mutMidiChannel(RuntimeConfig& c) { c.midiChannel = 7; }
void mutTimeUnpower(RuntimeConfig& c) { c.timeUnpower = 9000; }
void mutMidiStorageLimit(RuntimeConfig& c) { c.midiStorageLimitKb = 800; }
void mutHideAir(RuntimeConfig& c) { c.hideAir = true; }
void mutKbdMode(RuntimeConfig& c) { c.kbdMode = 1; }
void mutInstrumentColor(RuntimeConfig& c) { setText(c.instrumentColor, sizeof(c.instrumentColor), "#112233"); }
void mutEmbouchure(RuntimeConfig& c) { setText(c.embouchure, sizeof(c.embouchure), "bec"); }
void mutDeviceName(RuntimeConfig& c) { setText(c.deviceName, sizeof(c.deviceName), "flute-salon"); }
void mutWifiSsid(RuntimeConfig& c) { setText(c.wifiSsid, sizeof(c.wifiSsid), "atelier"); }

// Dans une fonction, pour la meme raison que topologyCases().
size_t musicalCases(const MusicalCase** out) {
  static const MusicalCase kCases[] = {
    { "ccVolumeDefault",               &mutCcVolume },
    { "ccBreathDefault",               &mutCcBreath },
    { "notes[].airflowNominalPercent", &mutNoteAirflow },
    { "numNotes",                      &mutNumNotes },
    { "fingers[].closedAngle",         &mutClosedAngle },
    { "fingers[].direction",           &mutFingerDirection },
    { "halfHolePercent",               &mutHalfHolePercent },
    { "servoAirflowMax",               &mutServoAirflowMax },
    { "vibratoFrequencyHz",            &mutVibratoHz },
    { "airAttackMs",                   &mutAirAttackMs },
    { "solenoidPwmHolding",            &mutSolenoidPwmHolding },
    { "fanMaxPwm",                     &mutFanMaxPwm },
    { "pumpMaxPwm[]",                  &mutPumpMaxPwm },
    { "pumpStaggerMs",                 &mutPumpStagger },
    { "pidKp",                         &mutPidKp },
    { "sensorTargetMm",                &mutSensorTargetMm },
    { "hallThresholdHigh",             &mutHallThreshold },
    { "endstopPumpOn",                 &mutEndstopPumpOn },
    { "reservoirTargetPercent",        &mutReservoirTarget },
    { "reservoirAutoStart",            &mutReservoirAutoStart },
    { "midiChannel",                   &mutMidiChannel },
    { "timeUnpower",                   &mutTimeUnpower },
    { "midiStorageLimitKb",            &mutMidiStorageLimit },
    { "hideAir",                       &mutHideAir },
    { "kbdMode",                       &mutKbdMode },
    { "instrumentColor",               &mutInstrumentColor },
    { "embouchure",                    &mutEmbouchure },
    { "deviceName",                    &mutDeviceName },
    { "wifiSsid",                      &mutWifiSsid },
  };
  *out = kCases;
  return sizeof(kCases) / sizeof(kCases[0]);
}

// LE contre-test de couverture. Une table trop large est un defaut aussi grave
// qu'une table trop etroite : si un reglage musical y entrait, la moindre
// modification de volume couperait le son le temps d'un redemarrage. Chacun de
// ces champs est relu a chaque note, a chaque boucle de regulation ou a chaque
// rendu de page - rien de materiel n'est a reconfigurer.
void fin_storage_topology_musical_changes_never_reboot() {
  RuntimeConfig base;
  topologyBaseline(base);

  const MusicalCase* cases = nullptr;
  const size_t n = musicalCases(&cases);
  assert(n >= 25);   // une poignee de champs ne prouverait pas grand-chose
  for (size_t i = 0; i < n; i++) {
    RuntimeConfig next = base;
    cases[i].mutate(next);
    // La mutation change bien quelque chose : sans cela le test serait vide.
    assert(memcmp(&base, &next, sizeof(RuntimeConfig)) != 0);
    assert(!configTopologyRequiresRestart(base, next));
    assert(configTopologyChangedField(base, next) == nullptr);
  }

  // Et toutes ensemble : l'utilisateur peut refaire l'integralite de ses
  // reglages musicaux et d'interface en une seule sauvegarde sans redemarrer.
  RuntimeConfig all = base;
  for (size_t i = 0; i < n; i++) {
    cases[i].mutate(all);
  }
  assert(memcmp(&base, &all, sizeof(RuntimeConfig)) != 0);
  assert(!configTopologyRequiresRestart(base, all));
}

// Les champs que la disjonction d'InstrumentManager couvrait DEJA continuent de
// declencher un redemarrage : la table doit etre un sur-ensemble, jamais un
// remplacement plus laxiste.
void fin_storage_topology_keeps_every_previously_covered_field() {
  RuntimeConfig base;
  topologyBaseline(base);

  static const char* const kPreviouslyCovered[] = {
    "numFingers", "numPumps", "airMode", "sensorType", "serialMidiEnabled",
    "serialMidiRxPin", "airflowPcaChannel", "valveServoPcaChannel",
    "angleServoPcaChannel", "solenoidPin", "fanPin", "fingers[].pcaChannel",
    "pumpPins[]",
  };
  const size_t n = sizeof(kPreviouslyCovered) / sizeof(kPreviouslyCovered[0]);

  for (size_t i = 0; i < n; i++) {
    bool found = false;
    for (size_t j = 0; j < configTopologyFieldCount() && !found; j++) {
      found = strcmp(configTopologyFieldAt(j).name, kPreviouslyCovered[i]) == 0;
    }
    assert(found);
  }

  // Et le comportement, pas seulement les noms : un changement de compte de
  // pompes exige toujours un redemarrage.
  RuntimeConfig next = base;
  next.numPumps = 1;
  assert(configTopologyRequiresRestart(base, next));
}

}  // namespace

void fin_storage_run_all_tests() {
  fin_storage_failed_save_is_never_applied_on_next_boot();
  fin_storage_lone_tmp_is_still_promoted();
  fin_storage_live_config_wins_over_leftovers();
  fin_storage_failed_replace_then_boot_returns_the_previous_config();
  fin_storage_unmovable_backup_never_falls_back_to_the_candidate();
  fin_storage_empty_filesystem_recovers_nothing();

  fin_storage_legacy_factory_reset_destroys_then_reports_failure();
  fin_storage_factory_reset_nominal();
  fin_storage_factory_reset_failures_never_lose_the_config();
  fin_storage_factory_reset_leaves_nothing_to_recover();

  fin_storage_topology_identical_configs_need_no_restart();
  fin_storage_topology_covers_the_three_missing_pins();
  fin_storage_topology_every_table_entry_is_reachable();
  fin_storage_topology_table_and_tests_know_each_other();
  fin_storage_topology_musical_changes_never_reboot();
  fin_storage_topology_keeps_every_previously_covered_field();

  std::cout << "fin storage tests passed\n";
}

#ifdef STANDALONE_TEST_MAIN
int main() { fin_storage_run_all_tests(); return 0; }
#endif

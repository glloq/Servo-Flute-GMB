/***********************************************************************************************
 * test_harden_storage - Durcissement de la persistance de configuration.
 *
 * Deux defauts, tous deux REPRODUITS en executant du code avant d'etre corriges :
 *
 *  A-1 : reset et reset usine ecrasaient la configuration ACTIVE (`cfg`) en RAM.
 *        `cfg` decrit le materiel reellement initialise (canaux PCA, broches,
 *        angles, sens de rotation) ; le remplacer pendant que loop() tourne fait
 *        piloter les actionneurs avec une description qui n'est plus celle du
 *        montage cable. Le reset usine etait le pire cas : il detruisait la
 *        configuration active SANS RIEN persister.
 *
 *  A-2 : la sauvegarde "atomique" pouvait detruire les DEUX copies. Elle
 *        supprimait la configuration courante AVANT le rename, puis supprimait le
 *        .tmp quand ce rename echouait - il ne restait alors plus rien a
 *        recuperer au demarrage.
 *
 * Point d'entree : harden_storage_run_all_tests().
 ***********************************************************************************************/
void harden_storage_run_all_tests();

#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>

#include "Arduino.h"
#include "ConfigStorage.h"
#include "ConfigPersist.h"

namespace {

const char* const kFinal = CONFIG_FILE_PATH;
const char* const kTmp = CONFIG_FILE_PATH ".tmp";
const char* const kBak = CONFIG_FILE_PATH ".bak";

// =============================================================================
// Faux systeme de fichiers en memoire
//
// Le stub LittleFS du depot est en lecture seule (pas de rename, pas d'ecriture)
// et la sequence de remplacement n'a de toute facon pas besoin de LittleFS : elle
// recoit ses operations par injection. Ce faux systeme de fichiers compte les
// appels et sait echouer, globalement ou sur une operation precise.
// =============================================================================

struct FakeFs {
  std::map<std::string, std::string> files;
  int existsCalls = 0;
  int removeCalls = 0;
  int renameCalls = 0;

  bool failAllRemoves = false;
  bool failAllRenames = false;
  bool lieExistsAlwaysTrue = false;   // systeme de fichiers qui repond n'importe quoi
  std::string failRemovePath;                 // echec cible d'un remove
  std::string failRenameFrom, failRenameTo;   // echec cible d'un rename precis

  bool has(const char* p) const { return files.count(p) != 0; }
  std::string get(const char* p) const {
    auto it = files.find(p);
    return it == files.end() ? std::string() : it->second;
  }
  void put(const char* p, const char* content) { files[p] = content; }
  size_t count() const { return files.size(); }
};

bool fakeExists(void* ctx, const char* path) {
  FakeFs* fs = static_cast<FakeFs*>(ctx);
  fs->existsCalls++;
  if (fs->lieExistsAlwaysTrue) return true;
  return fs->files.count(path) != 0;
}

bool fakeRemove(void* ctx, const char* path) {
  FakeFs* fs = static_cast<FakeFs*>(ctx);
  fs->removeCalls++;
  if (fs->failAllRemoves) return false;
  if (!fs->failRemovePath.empty() && fs->failRemovePath == path) return false;
  return fs->files.erase(path) > 0;
}

bool fakeRename(void* ctx, const char* from, const char* to) {
  FakeFs* fs = static_cast<FakeFs*>(ctx);
  fs->renameCalls++;
  if (fs->failAllRenames) return false;
  if (!fs->failRenameFrom.empty() && fs->failRenameFrom == from && fs->failRenameTo == to) {
    return false;
  }
  if (std::strcmp(from, to) == 0) return fs->files.count(from) != 0;
  auto it = fs->files.find(from);
  if (it == fs->files.end()) return false;
  fs->files[to] = it->second;
  fs->files.erase(it);
  return true;
}

FsRenameOps opsFor(FakeFs& fs) {
  FsRenameOps ops;
  ops.exists = &fakeExists;
  ops.remove = &fakeRemove;
  ops.rename = &fakeRename;
  ops.ctx = &fs;
  return ops;
}

// Sequence de remplacement TELLE QU'ELLE ETAIT dans ConfigStorage.cpp avant la
// correction (§"Replace the live config with the validated temp file") :
//     LittleFS.remove(CONFIG_FILE_PATH);
//     if (!LittleFS.rename(tmpPath, CONFIG_FILE_PATH)) { LittleFS.remove(tmpPath); return false; }
// Elle est conservee ici, et seulement ici, pour que le defaut reste demontre par
// du code execute et non par un commentaire.
bool legacyReplace(FakeFs& fs, const char* tmpPath, const char* finalPath) {
  fakeRemove(&fs, finalPath);
  if (!fakeRename(&fs, tmpPath, finalPath)) {
    fakeRemove(&fs, tmpPath);
    return false;
  }
  return true;
}

// --- Lecture des sources de production ---------------------------------------
// ConfigStorage.cpp n'est dans AUCUNE compilation hote (ArduinoJson, LittleFS) :
// ce qui s'y passe ne peut etre verrouille que textuellement. Les fonctions pures
// qu'il appelle, elles, sont executees pour de vrai plus haut.
std::string readProductionSource(const std::string& relative) {
  static const char* prefixes[] = { "", "../", "../../", "../../../", "../../../../" };
  for (const char* prefix : prefixes) {
    std::ifstream in(std::string(prefix) + relative);
    if (in) {
      std::ostringstream ss;
      ss << in.rdbuf();
      return ss.str();
    }
  }
  return std::string();
}

std::string sliceBetween(const std::string& text, const std::string& from, const std::string& to) {
  size_t a = text.find(from);
  assert(a != std::string::npos);
  size_t b = text.find(to, a);
  assert(b != std::string::npos);
  return text.substr(a, b - a);
}

// Une configuration active reconnaissable, qui joue le role du "materiel
// reellement cable". memset d'abord : RuntimeConfig contient des tableaux et des
// char[], donc du bourrage (padding) entre champs ; le mettre a zero est ce qui
// rend les comparaisons memcmp de ces tests stables.
void wiredHardwareCfg(RuntimeConfig& c) {
  memset(&c, 0, sizeof(c));
  c.numFingers = 3;
  c.numNotes = 5;
  c.fingers[0].pcaChannel = 7;
  c.fingers[0].closedAngle = 123;
  c.fingers[0].direction = -1;
  c.fingers[1].pcaChannel = 9;
  c.fingers[1].closedAngle = 41;
  c.fingers[1].direction = 1;
  c.notes[0].midiNote = 62;
  c.airflowPcaChannel = 11;
  c.solenoidPin = 19;
  c.airMode = 4;
  c.numPumps = 2;
  c.pumpPins[0] = 25;
  c.pumpPins[1] = 26;
  strncpy(c.deviceName, "flute-atelier", sizeof(c.deviceName) - 1);
  strncpy(c.embouchure, "bec", sizeof(c.embouchure) - 1);
}

// =============================================================================
// A-1 - preparer les defauts ne doit pas toucher la configuration ACTIVE
// =============================================================================

// AVANT : resetToDefaults() et factoryReset() appelaient initDefaults(), qui
// ecrit `cfg`. Sur le code d'origine, ce test echouait sur le memcmp (execute :
// `cfg` etait bien reecrit par le seul chemin de preparation disponible).
void harden_storage_defaults_never_touch_the_active_config() {
  wiredHardwareCfg(cfg);
  RuntimeConfig before;
  memcpy(&before, &cfg, sizeof(RuntimeConfig));

  RuntimeConfig out;
  memset(&out, 0xA5, sizeof(out));
  ConfigStorage::makeDefaultConfig(out);

  // 1. La configuration active est intacte, OCTET POUR OCTET.
  assert(memcmp(&cfg, &before, sizeof(RuntimeConfig)) == 0);

  // 2. Et `out` a vraiment ete rempli (sinon le test ci-dessus serait vide de sens).
  assert(out.numFingers == DEFAULT_NUM_FINGERS);
  assert(out.numNotes == DEFAULT_NUM_NOTES);
  assert(strcmp(out.embouchure, "trav") == 0);
  assert(strcmp(out.resFormat, "balloon") == 0);
  assert(out.airflowPcaChannel == DEFAULT_AIRFLOW_PCA_CHANNEL);
  // 3. Les defauts sont bien DIFFERENTS de la configuration cablee : c'est
  //    exactement pour cela que les ecraser en RAM etait dangereux.
  assert(memcmp(&out, &cfg, sizeof(RuntimeConfig)) != 0);
}

// initDefaults() ecrit toujours `cfg` : c'est voulu, et c'est reserve au chemin de
// BOOT (avant toute concurrence). Ce test verrouille cette semantique - c'est elle
// qui explique pourquoi les chemins de reset ne doivent plus l'appeler.
void harden_storage_init_defaults_stays_the_boot_path() {
  wiredHardwareCfg(cfg);
  RuntimeConfig wired;
  memcpy(&wired, &cfg, sizeof(RuntimeConfig));

  ConfigStorage::initDefaults();
  assert(memcmp(&cfg, &wired, sizeof(RuntimeConfig)) != 0);   // il ECRIT bien cfg

  // ... et il ecrit exactement ce que makeDefaultConfig() produit.
  RuntimeConfig expected;
  memset(&expected, 0x5A, sizeof(expected));
  ConfigStorage::makeDefaultConfig(expected);
  assert(memcmp(&cfg, &expected, sizeof(RuntimeConfig)) == 0);
}

// Deterministe et sans etat : deux appels sur deux objets au contenu initial
// DIFFERENT donnent le meme resultat, bourrage compris (c'est le memset en tete
// de makeDefaultConfig qui le garantit).
void harden_storage_defaults_are_deterministic_and_stateless() {
  wiredHardwareCfg(cfg);
  RuntimeConfig before;
  memcpy(&before, &cfg, sizeof(RuntimeConfig));

  RuntimeConfig a, b;
  memset(&a, 0x00, sizeof(a));
  memset(&b, 0xFF, sizeof(b));
  ConfigStorage::makeDefaultConfig(a);
  ConfigStorage::makeDefaultConfig(b);
  assert(memcmp(&a, &b, sizeof(RuntimeConfig)) == 0);

  // Idempotent : un second appel sur le meme objet ne derive pas.
  ConfigStorage::makeDefaultConfig(a);
  assert(memcmp(&a, &b, sizeof(RuntimeConfig)) == 0);

  // Aucun de ces appels n'a touche la configuration active.
  assert(memcmp(&cfg, &before, sizeof(RuntimeConfig)) == 0);
}

// =============================================================================
// A-2 - une sauvegarde ratee ne doit jamais detruire les deux copies
// =============================================================================

// Cas nominal : la nouvelle configuration prend la place, sans residu.
void harden_storage_atomic_replace_nominal() {
  FakeFs fs;
  fs.put(kFinal, "OLD");
  fs.put(kTmp, "NEW");

  assert(configAtomicReplace(opsFor(fs), kTmp, kFinal, kBak));
  assert(fs.get(kFinal) == "NEW");
  assert(!fs.has(kTmp));
  assert(!fs.has(kBak));
  assert(fs.count() == 1);
}

// Premier enregistrement (aucune configuration en place) : rien a sauvegarder,
// la promotion doit quand meme avoir lieu.
void harden_storage_atomic_replace_on_a_blank_filesystem() {
  FakeFs fs;
  fs.put(kTmp, "FIRST");

  assert(configAtomicReplace(opsFor(fs), kTmp, kFinal, kBak));
  assert(fs.get(kFinal) == "FIRST");
  assert(fs.count() == 1);
}

// LE test du defaut A-2. La sequence d'origine est rejouee sur le meme scenario
// pour montrer ce qu'elle perdait, puis la nouvelle est exigee de tenir.
void harden_storage_failed_promotion_keeps_a_readable_config() {
  // --- 1. Sequence d'ORIGINE : le rename echoue -> plus rien n'existe ---------
  {
    FakeFs fs;
    fs.put(kFinal, "OLD");
    fs.put(kTmp, "NEW");
    fs.failAllRenames = true;

    assert(!legacyReplace(fs, kTmp, kFinal));
    // Les DEUX copies ont disparu : ni configuration, ni rien a recuperer au
    // demarrage. C'est le defaut, execute.
    assert(!fs.has(kFinal));
    assert(!fs.has(kTmp));
    assert(fs.count() == 0);
    FakeFs empty = fs;
    assert(!configRecoverOnBoot(opsFor(empty), kTmp, kFinal, kBak));
  }

  // --- 2. Sequence CORRIGEE, meme scenario ----------------------------------
  {
    FakeFs fs;
    fs.put(kFinal, "OLD");
    fs.put(kTmp, "NEW");
    // Seul le rename qui PROMEUT echoue (la mise de cote en .bak passe).
    fs.failRenameFrom = kTmp;
    fs.failRenameTo = kFinal;

    assert(!configAtomicReplace(opsFor(fs), kTmp, kFinal, kBak));

    // Une configuration lisible subsiste, et c'est l'ancienne, restauree.
    assert(fs.has(kFinal));
    assert(fs.get(kFinal) == "OLD");

    // Et un demarrage maintenant retrouve cette meme configuration.
    assert(configRecoverOnBoot(opsFor(fs), kTmp, kFinal, kBak));
    assert(fs.get(kFinal) == "OLD");
  }

  // --- 3. Pire cas : meme la restauration echoue ----------------------------
  {
    FakeFs fs;
    fs.put(kFinal, "OLD");
    fs.put(kTmp, "NEW");
    // Mise de cote OK, puis plus AUCUN rename ne fonctionne : ni la promotion, ni
    // la restauration. L'ancienne configuration reste dans le .bak.
    FsRenameOps ops = opsFor(fs);
    assert(ops.rename(ops.ctx, kFinal, kBak));   // etape 2 jouee a la main
    fs.failAllRenames = true;
    assert(!configAtomicReplace(ops, kTmp, kFinal, kBak));
    assert(!fs.has(kFinal));
    assert(fs.get(kBak) == "OLD");
    assert(fs.get(kTmp) == "NEW");

    // Le demarrage suivant remet une configuration en place : c'est l'ANCIENNE.
    // configAtomicReplace() vient de rendre false - la sauvegarde a donc ete
    // ANNONCEE ECHOUEE a l'utilisateur. Promouvoir le .tmp ici appliquerait au
    // reboot une configuration que l'appareil a refusee : une operation ratee
    // deviendrait reussie, un redemarrage plus tard. Le .bak n'existe que parce
    // qu'une configuration COMMITEE y a ete mise de cote : c'est elle qui prime.
    fs.failAllRenames = false;
    assert(configRecoverOnBoot(opsFor(fs), kTmp, kFinal, kBak));
    assert(fs.get(kFinal) == "OLD");
    assert(!fs.has(kBak));
    assert(!fs.has(kTmp));
  }
}

// Echec du rename final -> .bak : rien n'a encore bouge, tout doit rester en place.
void harden_storage_failed_backup_leaves_everything_in_place() {
  FakeFs fs;
  fs.put(kFinal, "OLD");
  fs.put(kTmp, "NEW");
  fs.failRenameFrom = kFinal;
  fs.failRenameTo = kBak;

  assert(!configAtomicReplace(opsFor(fs), kTmp, kFinal, kBak));
  assert(fs.get(kFinal) == "OLD");   // la configuration courante n'a pas bouge
  assert(fs.get(kTmp) == "NEW");     // le candidat attend toujours
  assert(!fs.has(kBak));

  // Un demarrage ici garde l'ancienne configuration et jette le .tmp perime.
  assert(configRecoverOnBoot(opsFor(fs), kTmp, kFinal, kBak));
  assert(fs.get(kFinal) == "OLD");
  assert(!fs.has(kTmp));
}

// Un .bak d'une passe precedente qui refuse de disparaitre arrete la sequence
// AVANT tout degat.
void harden_storage_stale_backup_stops_before_any_damage() {
  FakeFs fs;
  fs.put(kFinal, "OLD");
  fs.put(kTmp, "NEW");
  fs.put(kBak, "ANCIEN");
  fs.failRemovePath = kBak;

  assert(!configAtomicReplace(opsFor(fs), kTmp, kFinal, kBak));
  assert(fs.get(kFinal) == "OLD");
  assert(fs.get(kTmp) == "NEW");
  assert(fs.get(kBak) == "ANCIEN");
}

// Rien a promouvoir : ne rien detruire pour autant.
void harden_storage_replace_without_tmp_is_a_refusal_not_a_deletion() {
  FakeFs fs;
  fs.put(kFinal, "OLD");

  assert(!configAtomicReplace(opsFor(fs), kTmp, kFinal, kBak));
  assert(fs.get(kFinal) == "OLD");
  assert(fs.renameCalls == 0);
  assert(fs.removeCalls == 0);
}

// =============================================================================
// Recuperation au demarrage
// =============================================================================

void harden_storage_recover_on_boot_cases() {
  // 1. Configuration finale absente, .tmp present -> promu.
  {
    FakeFs fs;
    fs.put(kTmp, "NEW");
    assert(configRecoverOnBoot(opsFor(fs), kTmp, kFinal, kBak));
    assert(fs.get(kFinal) == "NEW");
    assert(fs.count() == 1);
  }

  // 2. Configuration finale absente, .bak present -> promu.
  {
    FakeFs fs;
    fs.put(kBak, "OLD");
    assert(configRecoverOnBoot(opsFor(fs), kTmp, kFinal, kBak));
    assert(fs.get(kFinal) == "OLD");
    assert(fs.count() == 1);
  }

  // 3. Les deux residus : le .bak gagne. Sa seule origine est l'etape 2 de
  //    configAtomicReplace(), qui y deplace une configuration COMMITEE ; un .tmp
  //    qui survit a une configuration finale absente est un candidat dont la
  //    promotion a echoue. Le .tmp est efface avec lui.
  {
    FakeFs fs;
    fs.put(kTmp, "NEW");
    fs.put(kBak, "OLD");
    assert(configRecoverOnBoot(opsFor(fs), kTmp, kFinal, kBak));
    assert(fs.get(kFinal) == "OLD");
    assert(fs.count() == 1);
  }

  // 4. Configuration finale presente : on n'y touche pas, on nettoie les residus.
  {
    FakeFs fs;
    fs.put(kFinal, "LIVE");
    fs.put(kTmp, "STALE");
    fs.put(kBak, "OLD");
    assert(configRecoverOnBoot(opsFor(fs), kTmp, kFinal, kBak));
    assert(fs.get(kFinal) == "LIVE");
    assert(!fs.has(kTmp));
    assert(!fs.has(kBak));
    assert(fs.renameCalls == 0);
  }

  // 5. Systeme de fichiers vide (premier demarrage reel, ou reset usine complet) :
  //    rien a recuperer, et surtout rien de cree.
  {
    FakeFs fs;
    assert(!configRecoverOnBoot(opsFor(fs), kTmp, kFinal, kBak));
    assert(fs.count() == 0);
  }

  // 6. Le .tmp existe mais son rename echoue : on se rabat sur le .bak.
  {
    FakeFs fs;
    fs.put(kTmp, "NEW");
    fs.put(kBak, "OLD");
    fs.failRenameFrom = kTmp;
    fs.failRenameTo = kFinal;
    assert(configRecoverOnBoot(opsFor(fs), kTmp, kFinal, kBak));
    assert(fs.get(kFinal) == "OLD");
  }
}

// Systeme de fichiers qui echoue a TOUT : les deux fonctions doivent rendre la
// main, sans boucler ni ecrire n'importe quoi.
void harden_storage_dead_filesystem_never_loops() {
  // a. Tout est absent et toute mutation echoue.
  {
    FakeFs fs;
    fs.failAllRemoves = true;
    fs.failAllRenames = true;
    assert(!configAtomicReplace(opsFor(fs), kTmp, kFinal, kBak));
    assert(!configRecoverOnBoot(opsFor(fs), kTmp, kFinal, kBak));
    assert(fs.count() == 0);
  }

  // b. Le systeme de fichiers pretend que tout existe et refuse toute mutation.
  {
    FakeFs fs;
    fs.lieExistsAlwaysTrue = true;
    fs.failAllRemoves = true;
    fs.failAllRenames = true;
    assert(!configAtomicReplace(opsFor(fs), kTmp, kFinal, kBak));
    // configRecoverOnBoot voit une configuration en place : il le dit, sans avoir
    // rien pu nettoyer. C'est un constat, pas une boucle.
    assert(configRecoverOnBoot(opsFor(fs), kTmp, kFinal, kBak));
    assert(fs.count() == 0);
  }

  // c. Jeu d'operations incomplet : refus franc plutot qu'un saut dans le vide.
  {
    FakeFs fs;
    fs.put(kTmp, "NEW");
    FsRenameOps broken = opsFor(fs);
    broken.rename = nullptr;
    assert(!configAtomicReplace(broken, kTmp, kFinal, kBak));
    assert(!configRecoverOnBoot(broken, kTmp, kFinal, kBak));
    assert(fs.get(kTmp) == "NEW");
  }
}

// =============================================================================
// Verrou TEXTUEL sur ConfigStorage.cpp
//
// Ce fichier n'est dans aucune compilation hote : il ne peut etre ni execute ni
// compile ici. Les assertions ci-dessous verrouillent donc uniquement le fait que
// les chemins corriges appellent bien les fonctions pures testees plus haut.
// =============================================================================
void harden_storage_production_reset_paths_do_not_write_the_active_config() {
  std::string src = readProductionSource("Servo_flute_ESP32/ConfigStorage.cpp");
  assert(!src.empty());

  // A-1 : reset et reset usine ne preparent plus les defauts DANS `cfg`.
  std::string reset = sliceBetween(src, "bool ConfigStorage::resetToDefaults()",
                                   "bool ConfigStorage::factoryReset()");
  assert(reset.find("initDefaults()") == std::string::npos);
  assert(reset.find("makeDefaultConfig(") != std::string::npos);
  assert(reset.find("saveFrom(") != std::string::npos);

  std::string factory = sliceBetween(src, "bool ConfigStorage::factoryReset()",
                                     "bool ConfigStorage::isFirstBoot()");
  assert(factory.find("initDefaults()") == std::string::npos);
  // Le reset usine efface aussi les residus, sans quoi le demarrage suivant les
  // promouvrait et ressusciterait la configuration effacee.
  assert(factory.find("\".tmp\"") != std::string::npos);
  assert(factory.find("\".bak\"") != std::string::npos);

  // A-2 : le remplacement passe par la sequence pure, et la sequence destructrice
  // (suppression de la configuration courante avant le rename) a disparu.
  std::string write = sliceBetween(src, "static bool writeConfigFile(const RuntimeConfig& source)",
                                   "bool ConfigStorage::save()");
  assert(write.find("configAtomicReplace(") != std::string::npos);
  assert(write.find("LittleFS.remove(CONFIG_FILE_PATH)") == std::string::npos);

  // La recuperation au demarrage n'est pas dupliquee : elle appelle la meme
  // fonction pure.
  std::string load = sliceBetween(src, "ConfigLoadStatus ConfigStorage::loadWithStatus()",
                                  "static bool writeConfigFile");
  assert(load.find("configRecoverOnBoot(") != std::string::npos);

  // initDefaults() vit desormais dans ConfigDefaults.cpp (sans ArduinoJson ni
  // LittleFS), ce qui est la raison pour laquelle les tests ci-dessus existent.
  std::string defaults = readProductionSource("Servo_flute_ESP32/ConfigDefaults.cpp");
  assert(!defaults.empty());
  assert(defaults.find("#include <ArduinoJson.h>") == std::string::npos);
  assert(defaults.find("#include <LittleFS.h>") == std::string::npos);
  assert(defaults.find("LittleFS.") == std::string::npos);
  assert(defaults.find("void ConfigStorage::makeDefaultConfig(RuntimeConfig& out)") != std::string::npos);

  std::string persist = readProductionSource("Servo_flute_ESP32/ConfigPersist.cpp");
  assert(!persist.empty());
  assert(persist.find("#include <ArduinoJson.h>") == std::string::npos);
  assert(persist.find("#include <LittleFS.h>") == std::string::npos);
  assert(persist.find("LittleFS.") == std::string::npos);
}

}  // namespace

void harden_storage_run_all_tests() {
  // `cfg` est partage par toutes les unites de test natives : on le rend tel
  // qu'on l'a trouve pour ne rien decaler chez les suivantes.
  RuntimeConfig saved;
  memcpy(&saved, &cfg, sizeof(RuntimeConfig));

  harden_storage_defaults_never_touch_the_active_config();
  harden_storage_init_defaults_stays_the_boot_path();
  harden_storage_defaults_are_deterministic_and_stateless();
  harden_storage_atomic_replace_nominal();
  harden_storage_atomic_replace_on_a_blank_filesystem();
  harden_storage_failed_promotion_keeps_a_readable_config();
  harden_storage_failed_backup_leaves_everything_in_place();
  harden_storage_stale_backup_stops_before_any_damage();
  harden_storage_replace_without_tmp_is_a_refusal_not_a_deletion();
  harden_storage_recover_on_boot_cases();
  harden_storage_dead_filesystem_never_loops();
  harden_storage_production_reset_paths_do_not_write_the_active_config();

  memcpy(&cfg, &saved, sizeof(RuntimeConfig));
  std::cout << "harden storage tests passed\n";
}

#ifdef STANDALONE_TEST_MAIN
int main() { harden_storage_run_all_tests(); return 0; }
#endif

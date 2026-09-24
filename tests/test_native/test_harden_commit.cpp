/***********************************************************************************************
 * test_harden_commit - Le verrou du commit de configuration doit VERROUILLER.
 *
 * DEFAUT REPRODUIT
 * ----------------
 * commitCandidateConfig() prenait le verrou qui protege la configuration ACTIVE,
 * puis ecrivait `active = candidate` MEME quand lock() avait echoue : il se
 * contentait d'ajouter l'avertissement "config_lock_timeout". Or lock() echoue
 * precisement quand une autre tache lit ou ecrit `active`. `RuntimeConfig` fait
 * plusieurs kilo-octets : la recopie n'est pas atomique vis-a-vis d'un autre
 * coeur, et le lecteur concurrent voyait une structure mi-ancienne mi-nouvelle -
 * exactement ce que le verrou existe pour empecher. Un avertissement rend le
 * probleme visible sans l'empecher.
 *
 * INVARIANT VERROUILLE ICI
 * ------------------------
 * Il est INTERDIT d'ecrire `active` si le verrou n'est pas acquis. Le candidat
 * est deja en flash (la flash n'est pas la meme ressource que la RAM, et son
 * ecriture a lieu HORS verrou) : le resultat doit donc dire `saved` mais pas
 * `activated`, et demander le redemarrage qui reconciliera RAM et flash.
 *
 * Point d'entree : harden_commit_run_all_tests().
 ***********************************************************************************************/
#include <cassert>
#include <cstring>
#include <cstdio>
#include <iostream>
#include <string>

#include "Arduino.h"
#include "ConfigStorage.h"
#include "ConfigCommit.h"

namespace {

// --- Configuration de base VALIDE -------------------------------------------
// Construite par memset + affectations : les octets de bourrage restent a zero,
// ce qui rend la comparaison memcmp de commit_lock_refused_leaves_active_intact
// deterministe. Sa validite est verifiee par harden_commit_base_config_is_valid.
void hardenBase(RuntimeConfig& c) {
  memset(&c, 0, sizeof(c));
  c.numFingers = 1;
  c.fingers[0].pcaChannel = 0; c.fingers[0].closedAngle = 90; c.fingers[0].direction = 1;
  c.numPumps = 1; c.pumpPins[0] = 25; c.pumpMinPwm[0] = 80; c.pumpMaxPwm[0] = 200;
  c.motorType = MOTOR_TYPE_PWM; c.airMode = AIR_MODE_PUMP_VALVE;
  c.sensorType = SENSOR_TYPE_HALL_KY024; c.hallPin = 36;
  c.hallThresholdLow = 1000; c.hallThresholdHigh = 2000;
  c.pidKp = 10; c.pidKi = 0;
  c.servoToSolenoidDelayMs = 10; c.minNoteDurationMs = 100;
  c.airflowPcaChannel = 10; c.solenoidPin = 13;
  c.solenoidActivationTimeMs = 50; c.solenoidPwmActivation = 255; c.solenoidPwmHolding = 128;
  c.servoAirflowOff = 20; c.servoAirflowMin = 60; c.servoAirflowMax = 100;
  c.servoAngleOff = 90; c.servoAngleMin = 45; c.servoAngleMax = 135;
  c.ccVolumeDefault = 127; c.ccExpressionDefault = 127;
  c.ccBreathDefault = 127; c.ccBrightnessDefault = 64; c.airVelocityResponse = 100;
  strcpy(c.embouchure, "bec");
  c.numNotes = 3;
  c.notes[0].midiNote = 60; c.notes[1].midiNote = 62; c.notes[2].midiNote = 64;
  for (int i = 0; i < 3; i++) {
    c.notes[i].airflowMinPercent = 0;
    c.notes[i].airflowMaxPercent = 100;
    c.notes[i].airflowNominalPercent = 50;
  }
}

// --- Espion de persistance ---------------------------------------------------
bool g_saveOk = true;
int g_saveCalls = 0;
RuntimeConfig g_lastSaved;
bool hardenSave(const RuntimeConfig& c) {
  g_saveCalls++;
  if (!g_saveOk) return false;
  g_lastSaved = c;
  return true;
}

// --- Espion de verrou --------------------------------------------------------
// Compte SEPAREMENT les prises et les relachements : relacher un verrou qu'on
// n'a pas pris est une corruption classique (un mutex FreeRTOS rendu par une
// tache qui ne le detient pas), pas un detail cosmetique.
bool g_lockGrants = true;
int g_lockCalls = 0;
int g_unlockCalls = 0;
int g_ctxSeen = 0;
int g_ctxToken = 0;

bool hardenLock(void* ctx) {
  g_lockCalls++;
  if (ctx == &g_ctxToken) g_ctxSeen++;
  return g_lockGrants;
}
void hardenUnlock(void* ctx) {
  g_unlockCalls++;
  if (ctx == &g_ctxToken) g_ctxSeen++;
}

ConfigCommitGuard makeGuard() {
  return ConfigCommitGuard{ &hardenLock, &hardenUnlock, &g_ctxToken };
}

void resetSpies(bool saveOk, bool lockGrants) {
  g_saveOk = saveOk; g_saveCalls = 0;
  memset(&g_lastSaved, 0, sizeof(g_lastSaved));
  g_lockGrants = lockGrants; g_lockCalls = 0; g_unlockCalls = 0; g_ctxSeen = 0;
}

bool warns(const ConfigCommitResult& r, const char* needle) {
  return r.warnings.find(needle) != std::string::npos;
}

// Un changement PUREMENT logiciel : il ne declenche jamais
// InstrumentManager::configChangeRequiresRestart(), donc il atteint bien la
// section de commit atomique - c'est la seule qui touche `active`.
void makeSoftCandidate(RuntimeConfig& candidate, const RuntimeConfig& active) {
  candidate = active;
  candidate.ccVolumeDefault = 64;
  candidate.ccExpressionDefault = 100;
}

// =============================================================================
// Garde-fou : sans configuration de base valide, aucun test ci-dessous ne
// prouverait quoi que ce soit (tout sortirait par le chemin "invalide").
// =============================================================================
void harden_commit_base_config_is_valid() {
  RuntimeConfig c;
  hardenBase(c);
  ConfigValidationResult v = validateAndNormalizeConfig(c, nullptr);
  assert(v.valid);
  // Une base deja normalisee : le candidat derive n'est corrige par personne,
  // donc une difference observee plus bas vient bien du commit.
  RuntimeConfig again = c;
  ConfigValidationResult v2 = validateAndNormalizeConfig(again, &c);
  assert(v2.valid && !v2.corrected);
  assert(memcmp(&again, &c, sizeof(RuntimeConfig)) == 0);
}

// =============================================================================
// 1. LE TEST DU DEFAUT - verrou refuse => `active` est BIT A BIT inchange.
// =============================================================================
// Sur le code d'avant la correction, `active = candidate` etait execute quand
// meme : cette assertion echouait.
void harden_commit_lock_refused_leaves_active_bit_identical() {
  RuntimeConfig active;
  hardenBase(active);
  RuntimeConfig before;
  memcpy(&before, &active, sizeof(RuntimeConfig));   // bourrage inclus

  RuntimeConfig candidate;
  makeSoftCandidate(candidate, active);

  resetSpies(true, false);   // la flash accepte, le verrou REFUSE
  const ConfigCommitGuard guard = makeGuard();
  ConfigCommitResult r = commitCandidateConfig(active, candidate, nullptr, &hardenSave, &guard);

  // L'assertion centrale : pas un octet de la configuration active n'a bouge.
  assert(memcmp(&active, &before, sizeof(RuntimeConfig)) == 0);
  // Meme constat champ par champ, pour que l'echec soit lisible si le memcmp
  // ci-dessus venait a dependre du bourrage de la structure.
  assert(active.ccVolumeDefault == before.ccVolumeDefault);
  assert(active.ccExpressionDefault == before.ccExpressionDefault);
  assert(active.ccVolumeDefault != candidate.ccVolumeDefault);
  // La flash, elle, a bien recu le candidat : ce n'est pas la meme ressource.
  assert(r.saved);
  assert(g_saveCalls == 1);
  assert(g_lastSaved.ccVolumeDefault == 64);
}

// =============================================================================
// 2. Verrou refuse => le resultat le DIT, et permet le redemarrage controle.
// =============================================================================
void harden_commit_lock_refused_reports_not_activated() {
  RuntimeConfig active;
  hardenBase(active);
  RuntimeConfig candidate;
  makeSoftCandidate(candidate, active);

  resetSpies(true, false);
  const ConfigCommitGuard guard = makeGuard();
  ConfigCommitResult r = commitCandidateConfig(active, candidate, nullptr, &hardenSave, &guard);

  assert(r.valid);                 // le candidat etait bon
  assert(r.saved);                 // et il est en flash
  assert(!r.activated);            // mais la RAM n'a PAS ete remplacee
  assert(!r.applied);              // donc rien n'a ete applique aux controleurs
  assert(r.error.length() == 0);   // ce n'est pas une erreur de stockage
  assert(warns(r, "config_lock_timeout"));
  // RAM et flash divergent : l'appelant doit pouvoir declencher un redemarrage
  // controle, qui est ce qui les reconcilie. C'est le champ que WebConfigurator
  // lit deja pour programmer ce redemarrage.
  assert(r.restartRequired);
}

// =============================================================================
// 3. Verrou refuse => unlock() n'est JAMAIS appele.
// =============================================================================
// Rendre un mutex qu'on ne detient pas corrompt son compteur : la prise
// suivante reussit alors qu'elle ne devrait pas, et la protection disparait
// silencieusement pour tout le monde.
void harden_commit_lock_refused_never_unlocks() {
  RuntimeConfig active;
  hardenBase(active);
  RuntimeConfig candidate;
  makeSoftCandidate(candidate, active);

  resetSpies(true, false);
  const ConfigCommitGuard guard = makeGuard();
  ConfigCommitResult r = commitCandidateConfig(active, candidate, nullptr, &hardenSave, &guard);

  assert(!r.activated);
  assert(g_lockCalls == 1);    // une seule tentative, pas de boucle de reprise
  assert(g_unlockCalls == 0);  // et aucun relachement
  assert(g_ctxSeen == 1);      // le contexte transmis est bien celui du garde
}

// =============================================================================
// 4. Verrou accorde => activation reelle, verrou relache exactement une fois.
// =============================================================================
void harden_commit_lock_granted_activates_under_the_lock() {
  RuntimeConfig active;
  hardenBase(active);
  RuntimeConfig candidate;
  makeSoftCandidate(candidate, active);
  RuntimeConfig expected = candidate;

  resetSpies(true, true);
  const ConfigCommitGuard guard = makeGuard();
  ConfigCommitResult r = commitCandidateConfig(active, candidate, nullptr, &hardenSave, &guard);

  assert(r.valid && r.saved && r.activated && r.applied && !r.restartRequired);
  assert(!warns(r, "config_lock_timeout"));
  // `active` vaut le candidat (comparaison champ par champ : l'affectation de
  // structure ne garantit pas la recopie du bourrage).
  assert(active.ccVolumeDefault == expected.ccVolumeDefault);
  assert(active.ccExpressionDefault == expected.ccExpressionDefault);
  assert(active.numNotes == expected.numNotes);
  assert(active.notes[1].midiNote == expected.notes[1].midiNote);
  assert(g_lockCalls == 1 && g_unlockCalls == 1);
  assert(g_ctxSeen == 2);
}

// =============================================================================
// 5. Aucun garde fourni => le comportement mono-tache est preserve.
// =============================================================================
// C'est le chemin qu'empruntent les tests existants de ConfigCommit
// (tests/test_native/test_audit.cpp, section 6) et tout appelant hors FreeRTOS.
void harden_commit_without_guard_still_activates() {
  // (a) aucun garde du tout
  RuntimeConfig active;
  hardenBase(active);
  RuntimeConfig candidate;
  makeSoftCandidate(candidate, active);
  resetSpies(true, false);   // g_lockGrants ne doit rien changer ici
  ConfigCommitResult r = commitCandidateConfig(active, candidate, nullptr, &hardenSave, nullptr);
  assert(r.valid && r.saved && r.activated && r.applied && !r.restartRequired);
  assert(active.ccVolumeDefault == 64);
  assert(!warns(r, "config_lock_timeout"));
  assert(g_lockCalls == 0 && g_unlockCalls == 0);

  // (b) un garde present mais sans fonctions : pas de verrou a prendre, donc
  // rien a refuser. Ne pas confondre "pas de verrou" et "verrou refuse".
  RuntimeConfig active2;
  hardenBase(active2);
  RuntimeConfig candidate2;
  makeSoftCandidate(candidate2, active2);
  resetSpies(true, false);
  const ConfigCommitGuard empty{ nullptr, nullptr, nullptr };
  ConfigCommitResult r2 = commitCandidateConfig(active2, candidate2, nullptr, &hardenSave, &empty);
  assert(r2.valid && r2.saved && r2.activated && r2.applied && !r2.restartRequired);
  assert(active2.ccVolumeDefault == 64);
  assert(!warns(r2, "config_lock_timeout"));
}

// =============================================================================
// 6. Candidat INVALIDE => `active` intact et `activated` faux, verrou ou pas.
// =============================================================================
void harden_commit_invalid_candidate_never_activates() {
  for (int grant = 0; grant <= 1; grant++) {
    RuntimeConfig active;
    hardenBase(active);
    RuntimeConfig before;
    memcpy(&before, &active, sizeof(RuntimeConfig));

    RuntimeConfig candidate = active;
    candidate.numNotes = 2;
    candidate.notes[0].midiNote = 60;
    candidate.notes[1].midiNote = 60;   // doublon -> invalide

    resetSpies(true, grant != 0);
    const ConfigCommitGuard guard = makeGuard();
    ConfigCommitResult r = commitCandidateConfig(active, candidate, nullptr, &hardenSave, &guard);

    assert(!r.valid);
    assert(!r.activated && !r.applied && !r.saved);
    assert(r.error.length() > 0);
    assert(memcmp(&active, &before, sizeof(RuntimeConfig)) == 0);
    // Un candidat refuse n'ecrit ni la flash ni la RAM : il n'y a donc aucune
    // raison de deranger l'autre tache avec une prise de verrou.
    assert(g_saveCalls == 0);
    assert(g_lockCalls == 0 && g_unlockCalls == 0);
  }
}

// =============================================================================
// 7. Le verrou est equilibre sur CHAQUE chemin de sortie, sorties anticipees
//    comprises. On compte les appels, on ne les suppose pas.
// =============================================================================
void harden_commit_lock_is_balanced_on_every_exit_path() {
  // (a) candidat invalide : sortie avant toute ecriture.
  {
    RuntimeConfig active; hardenBase(active);
    RuntimeConfig candidate = active;
    candidate.numNotes = 2;
    candidate.notes[0].midiNote = 60; candidate.notes[1].midiNote = 60;
    resetSpies(true, true);
    const ConfigCommitGuard guard = makeGuard();
    ConfigCommitResult r = commitCandidateConfig(active, candidate, nullptr, &hardenSave, &guard);
    assert(!r.valid && !r.activated);
    assert(g_lockCalls == 0 && g_unlockCalls == 0);
  }

  // (b) sauvegarde impossible : la flash a refuse, rien n'est active, et le
  // verrou n'a aucune raison d'avoir ete pris.
  {
    RuntimeConfig active; hardenBase(active);
    RuntimeConfig before; memcpy(&before, &active, sizeof(RuntimeConfig));
    RuntimeConfig candidate; makeSoftCandidate(candidate, active);
    resetSpies(false, true);
    const ConfigCommitGuard guard = makeGuard();
    ConfigCommitResult r = commitCandidateConfig(active, candidate, nullptr, &hardenSave, &guard);
    assert(r.valid && !r.saved && !r.activated && !r.applied);
    assert(r.error == "storage_failed");
    assert(memcmp(&active, &before, sizeof(RuntimeConfig)) == 0);
    assert(g_saveCalls == 1);
    assert(g_lockCalls == 0 && g_unlockCalls == 0);
  }

  // (c) changement demandant une re-init hardware : sauvegarde mais PAS active.
  // `active` doit continuer de decrire le hardware reellement initialise, donc
  // ce chemin non plus ne prend le verrou.
  {
    RuntimeConfig active; hardenBase(active);
    RuntimeConfig before; memcpy(&before, &active, sizeof(RuntimeConfig));
    RuntimeConfig candidate = active;
    candidate.solenoidPin = 27;          // changement de GPIO
    resetSpies(true, true);
    const ConfigCommitGuard guard = makeGuard();
    ConfigCommitResult r = commitCandidateConfig(active, candidate, nullptr, &hardenSave, &guard);
    assert(r.valid && r.saved && r.restartRequired);
    assert(!r.activated && !r.applied);
    assert(memcmp(&active, &before, sizeof(RuntimeConfig)) == 0);
    assert(g_saveCalls == 1);
    assert(g_lockCalls == 0 && g_unlockCalls == 0);
  }

  // (d) verrou refuse : pris une fois, JAMAIS relache.
  {
    RuntimeConfig active; hardenBase(active);
    RuntimeConfig candidate; makeSoftCandidate(candidate, active);
    resetSpies(true, false);
    const ConfigCommitGuard guard = makeGuard();
    ConfigCommitResult r = commitCandidateConfig(active, candidate, nullptr, &hardenSave, &guard);
    assert(!r.activated);
    assert(g_lockCalls == 1 && g_unlockCalls == 0);
  }

  // (e) chemin nominal : pris une fois, relache une fois. Jamais deux.
  {
    RuntimeConfig active; hardenBase(active);
    RuntimeConfig candidate; makeSoftCandidate(candidate, active);
    resetSpies(true, true);
    const ConfigCommitGuard guard = makeGuard();
    ConfigCommitResult r = commitCandidateConfig(active, candidate, nullptr, &hardenSave, &guard);
    assert(r.activated && r.applied);
    assert(g_lockCalls == 1 && g_unlockCalls == 1);
  }

  // (f) deux commits successifs sur le meme garde : les compteurs restent
  // apparies (un verrou fuite se verrait ici comme un ecart cumule).
  {
    RuntimeConfig active; hardenBase(active);
    resetSpies(true, true);
    const ConfigCommitGuard guard = makeGuard();
    for (int i = 0; i < 2; i++) {
      RuntimeConfig candidate = active;
      candidate.ccVolumeDefault = (uint8_t)(50 + i);
      ConfigCommitResult r = commitCandidateConfig(active, candidate, nullptr, &hardenSave, &guard);
      assert(r.activated);
    }
    assert(g_lockCalls == 2 && g_unlockCalls == 2);
  }
}

// =============================================================================
// 8. Une sauvegarde sans activation laisse la flash EN AVANCE sur la RAM.
// =============================================================================
// C'est la contrepartie assumee de l'invariant : on refuse d'ecrire `active`
// hors verrou, donc on accepte une divergence RAM/flash que seul le redemarrage
// reconcilie. Le test verrouille le fait que le resultat porte de quoi la
// detecter sans deviner (saved && !activated) ET de quoi la resoudre.
void harden_commit_lock_refused_exposes_the_ram_flash_divergence() {
  RuntimeConfig active;
  hardenBase(active);
  RuntimeConfig candidate;
  makeSoftCandidate(candidate, active);

  resetSpies(true, false);
  const ConfigCommitGuard guard = makeGuard();
  ConfigCommitResult r = commitCandidateConfig(active, candidate, nullptr, &hardenSave, &guard);

  assert(r.saved && !r.activated);                       // la divergence
  assert(g_lastSaved.ccVolumeDefault == 64);             // cote flash
  assert(active.ccVolumeDefault == 127);                 // cote RAM
  assert(r.restartRequired);                             // et de quoi la resoudre
}

}  // namespace

void harden_commit_run_all_tests() {
  harden_commit_base_config_is_valid();
  harden_commit_lock_refused_leaves_active_bit_identical();
  harden_commit_lock_refused_reports_not_activated();
  harden_commit_lock_refused_never_unlocks();
  harden_commit_lock_granted_activates_under_the_lock();
  harden_commit_without_guard_still_activates();
  harden_commit_invalid_candidate_never_activates();
  harden_commit_lock_is_balanced_on_every_exit_path();
  harden_commit_lock_refused_exposes_the_ram_flash_divergence();
  std::cout << "harden commit tests passed\n";
}

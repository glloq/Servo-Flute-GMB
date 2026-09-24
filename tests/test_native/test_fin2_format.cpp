// Finalisation 2 - LOT 4 : formater LittleFS sans se faire redemarrer par le
// chien de garde, et sans jamais le laisser desarme.
//
// LE DEFAUT, tel qu'il existe dans le depot
// -----------------------------------------
//     case WEBOP_FORMAT_FS:
//       if (_instrument) _instrument->allSoundOff();
//       bool ok = ConfigStorage::formatFilesystem();   // LittleFS.format()
//
// `LittleFS.format()` efface ~1,9 Mo, bloquant, en grande partie cache
// d'instructions desactive : `loop()` ne tourne pas, donc `esp_task_wdt_reset()`
// n'est pas appele, et le chien de garde de tache - arme a 4000 ms avec
// `trigger_panic = true` - redemarre la carte EN PLEIN FORMATAGE. C'est le
// chemin de recuperation documente pour une carte vierge : celui du premier
// bring-up, celui qui doit marcher du premier coup.
//
// CE QUI EST TESTE ICI, ET CE QUI NE PEUT PAS L'ETRE
// --------------------------------------------------
// Un test hote ne formate aucune flash et n'a pas de chien de garde. Ce qui est
// verifie est la SEQUENCE et ses invariants, avec des primitives injectees qui
// enregistrent leur ordre d'appel et peuvent echouer a volonte :
//
//   - le materiel est mis en securite AVANT toute modification du chien de
//     garde, et un echec de cette mise en securite arrete tout ;
//   - si la suspension du chien de garde echoue, le formatage N'A PAS LIEU ;
//   - il n'existe aucun chemin de retour qui laisse le chien de garde suspendu,
//     succes ou echec du formatage - et si la restauration echoue elle-meme,
//     l'appelant l'apprend.
//
// Ce que cela ne prouve pas : que `esp_task_wdt_delete()` suffit reellement a
// traverser un effacement de 1,9 Mo sur la carte. Seuls les deux builds ESP32
// attestent que ces appels existent et sont bien types ; l'essai FIN-FS-WDT de
// HARDWARE_TEST_MATRIX.md reste a faire.
//
// Niveau de validation atteint : EXECUTE SUR HOTE. Rien n'a tourne sur ESP32.
#include <cassert>
#include <cstring>
#include <string>
#include <algorithm>
#include <vector>
#include "FormatGuard.h"

namespace {

struct FakeFlash {
  std::vector<std::string> log;
  bool safeOk = true;
  bool suspendOk = true;
  bool restoreOk = true;
  bool formatOk = true;
  // Vrai si le chien de garde est suspendu A CET INSTANT. Sert a verifier que
  // le formatage se deroule bien pendant la suspension, et pas a cote.
  bool wdtSuspended = false;
  bool formattedWhileSuspended = false;
};

bool fakeSafe(void* c) {
  FakeFlash* f = static_cast<FakeFlash*>(c);
  f->log.push_back("safe");
  return f->safeOk;
}
bool fakeSuspend(void* c) {
  FakeFlash* f = static_cast<FakeFlash*>(c);
  f->log.push_back("suspend");
  if (!f->suspendOk) return false;
  f->wdtSuspended = true;
  return true;
}
bool fakeRestore(void* c) {
  FakeFlash* f = static_cast<FakeFlash*>(c);
  f->log.push_back("restore");
  if (!f->restoreOk) return false;
  f->wdtSuspended = false;
  return true;
}
bool fakeFormat(void* c) {
  FakeFlash* f = static_cast<FakeFlash*>(c);
  f->log.push_back("format");
  f->formattedWhileSuspended = f->wdtSuspended;
  return f->formatOk;
}

FormatGuardOps opsFor(FakeFlash& f) {
  FormatGuardOps o;
  o.safeHardware = fakeSafe;
  o.suspendWatchdog = fakeSuspend;
  o.restoreWatchdog = fakeRestore;
  o.format = fakeFormat;
  o.ctx = &f;
  return o;
}

bool logIs(const FakeFlash& f, const std::vector<std::string>& expected) {
  return f.log == expected;
}

void the_nominal_sequence_is_safe_suspend_format_restore() {
  FakeFlash f;
  FormatGuardOutcome out = formatGuarded(opsFor(f));
  assert(out.result == FMT_OK);
  assert(logIs(f, {"safe", "suspend", "format", "restore"}));
  // Le formatage a bien eu lieu PENDANT la suspension : c'est tout l'objet.
  assert(f.formattedWhileSuspended);
  assert(out.watchdogSuspended && out.watchdogRestored);
  assert(!f.wdtSuspended);
}

// REGLE 1 : materiel d'abord. Un echec arrete tout - on ne touche pas au chien
// de garde et on ne formate pas.
void an_unsafe_hardware_refuses_the_format_and_never_touches_the_watchdog() {
  FakeFlash f; f.safeOk = false;
  FormatGuardOutcome out = formatGuarded(opsFor(f));
  assert(out.result == FMT_UNSAFE_HARDWARE);
  assert(logIs(f, {"safe"}));
  assert(!out.watchdogSuspended);
  // Rien n'a ete suspendu, donc l'invariant "jamais laisse suspendu" tient.
  assert(out.watchdogRestored);
  assert(!f.wdtSuspended);
  assert(strcmp(formatGuardErrorCode(out.result), "hardware_not_safe") == 0);
}

// REGLE 2 : sans suspension, pas de formatage. Formater quand meme reviendrait
// a declencher sciemment le redemarrage qu'on cherche a eviter, au milieu d'un
// effacement de flash.
void a_watchdog_that_cannot_be_suspended_refuses_the_format() {
  FakeFlash f; f.suspendOk = false;
  FormatGuardOutcome out = formatGuarded(opsFor(f));
  assert(out.result == FMT_WDT_SUSPEND_FAILED);
  assert(logIs(f, {"safe", "suspend"}));
  assert(!out.watchdogSuspended);
  assert(out.watchdogRestored);
  assert(!f.wdtSuspended);
  assert(strcmp(formatGuardErrorCode(out.result), "watchdog_busy") == 0);
}

// REGLE 3, le coeur : un formatage RATE restaure quand meme le chien de garde.
// C'est le chemin qu'un `return` anticipe aurait oublie.
void a_failed_format_still_restores_the_watchdog() {
  FakeFlash f; f.formatOk = false;
  FormatGuardOutcome out = formatGuarded(opsFor(f));
  assert(out.result == FMT_FORMAT_FAILED);
  assert(logIs(f, {"safe", "suspend", "format", "restore"}));
  assert(out.watchdogSuspended && out.watchdogRestored);
  assert(!f.wdtSuspended);
  assert(strcmp(formatGuardErrorCode(out.result), "format_failed") == 0);
}

// Et si la RESTAURATION elle-meme echoue, on le DIT au lieu de le supposer :
// l'appelant peut alors forcer un redemarrage, qui re-arme le chien de garde.
void a_failed_restore_is_reported_not_hidden() {
  FakeFlash f; f.restoreOk = false;
  FormatGuardOutcome out = formatGuarded(opsFor(f));
  assert(out.result == FMT_OK);          // le formatage, lui, a reussi
  assert(out.watchdogSuspended);
  assert(!out.watchdogRestored);         // ... mais on ne le cache pas
  assert(logIs(f, {"safe", "suspend", "format", "restore"}));
}

void a_failed_format_and_a_failed_restore_report_both() {
  FakeFlash f; f.formatOk = false; f.restoreOk = false;
  FormatGuardOutcome out = formatGuarded(opsFor(f));
  assert(out.result == FMT_FORMAT_FAILED);
  assert(out.watchdogSuspended && !out.watchdogRestored);
  assert(logIs(f, {"safe", "suspend", "format", "restore"}));
}

// INVARIANT GLOBAL, balaye sur les 16 combinaisons des quatre primitives :
// quelle que soit la combinaison d'echecs, on ne sort JAMAIS avec le chien de
// garde suspendu sans que la restauration ait ete TENTEE, et `format` n'est
// jamais appele sans suspension prealable reussie.
void no_combination_of_failures_leaves_the_watchdog_suspended() {
  for (int mask = 0; mask < 16; mask++) {
    FakeFlash f;
    f.safeOk    = (mask & 1) == 0;
    f.suspendOk = (mask & 2) == 0;
    f.restoreOk = (mask & 4) == 0;
    f.formatOk  = (mask & 8) == 0;
    FormatGuardOutcome out = formatGuarded(opsFor(f));

    const bool restoreAttempted =
        std::find(f.log.begin(), f.log.end(), std::string("restore")) != f.log.end();
    const bool formatAttempted =
        std::find(f.log.begin(), f.log.end(), std::string("format")) != f.log.end();

    // Suspendu => la restauration a ete TENTEE.
    if (out.watchdogSuspended) assert(restoreAttempted);
    // Formate => la suspension avait reussi, et le formatage s'est fait dedans.
    if (formatAttempted) {
      assert(out.watchdogSuspended);
      assert(f.formattedWhileSuspended);
    }
    // Le chien de garde n'est laisse suspendu que si la restauration a ECHOUE,
    // et ce cas est alors signale.
    if (f.wdtSuspended) assert(!out.watchdogRestored);
    if (!out.watchdogRestored) assert(!f.restoreOk);
    // La mise en securite precede TOUJOURS toute action sur le chien de garde.
    if (!f.log.empty()) assert(f.log[0] == "safe");
  }
}

// Primitive manquante : erreur de programmation, refus net - pas un formatage
// a moitie garde.
void missing_primitives_are_refused() {
  FakeFlash f;
  FormatGuardOps o = opsFor(f);
  o.format = nullptr;
  FormatGuardOutcome out = formatGuarded(o);
  assert(out.result == FMT_BAD_OPS);
  assert(f.log.empty());
  assert(out.watchdogRestored);
}

}  // namespace

void fin2_format_run_all_tests() {
  the_nominal_sequence_is_safe_suspend_format_restore();
  an_unsafe_hardware_refuses_the_format_and_never_touches_the_watchdog();
  a_watchdog_that_cannot_be_suspended_refuses_the_format();
  a_failed_format_still_restores_the_watchdog();
  a_failed_restore_is_reported_not_hidden();
  a_failed_format_and_a_failed_restore_report_both();
  no_combination_of_failures_leaves_the_watchdog_suspended();
  missing_primitives_are_refused();
}

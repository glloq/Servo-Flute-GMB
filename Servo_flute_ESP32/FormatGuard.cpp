#include "FormatGuard.h"

FormatGuardOutcome formatGuarded(const FormatGuardOps& ops) {
  FormatGuardOutcome out;
  out.result = FMT_OK;
  out.watchdogSuspended = false;
  // Vrai par defaut, et c'est le bon defaut : tant que rien n'a ete suspendu,
  // il n'y a rien a restaurer et l'invariant "jamais laisse suspendu" tient.
  out.watchdogRestored = true;

  if (ops.safeHardware == nullptr || ops.suspendWatchdog == nullptr ||
      ops.restoreWatchdog == nullptr || ops.format == nullptr) {
    out.result = FMT_BAD_OPS;
    return out;
  }

  // REGLE 1 - le materiel d'abord. Un echec ici arrete tout : on ne touche pas
  // au chien de garde et on ne formate pas.
  if (!ops.safeHardware(ops.ctx)) {
    out.result = FMT_UNSAFE_HARDWARE;
    return out;
  }

  // REGLE 2 - sans suspension, pas de formatage. Formater avec le chien de
  // garde arme, c'est choisir le redemarrage en plein effacement de flash.
  if (!ops.suspendWatchdog(ops.ctx)) {
    out.result = FMT_WDT_SUSPEND_FAILED;
    return out;
  }
  out.watchdogSuspended = true;
  out.watchdogRestored = false;

  const bool formatted = ops.format(ops.ctx);

  // REGLE 3 - UN SEUL chemin de retour au-dela de ce point, et il restaure.
  // La valeur de retour du formatage est mise de cote AVANT, pour qu'aucune
  // sortie anticipee ne puisse s'intercaler entre les deux.
  out.watchdogRestored = ops.restoreWatchdog(ops.ctx);
  out.result = formatted ? FMT_OK : FMT_FORMAT_FAILED;
  return out;
}

const char* formatGuardErrorCode(FormatGuardResult r) {
  switch (r) {
    case FMT_OK:                return "";
    case FMT_BAD_OPS:           return "internal_error";
    case FMT_UNSAFE_HARDWARE:   return "hardware_not_safe";
    case FMT_WDT_SUSPEND_FAILED:return "watchdog_busy";
    case FMT_FORMAT_FAILED:     return "format_failed";
  }
  return "internal_error";
}

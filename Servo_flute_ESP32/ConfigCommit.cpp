#include "ConfigCommit.h"
#include "InstrumentManager.h"

ConfigCommitResult commitCandidateConfig(RuntimeConfig& active,
                                         RuntimeConfig& candidate,
                                         InstrumentManager* instrument,
                                         ConfigSaveFn save) {
  ConfigCommitResult out{false, false, false, false, false, false, "", "", ""};

  // --- 1. Validation complete du CANDIDAT (la config active n'a pas bouge) ---
  ConfigValidationResult validation = validateAndNormalizeConfig(candidate, &active);
  out.corrected = validation.corrected;
  out.warnings = validation.warnings;
  if (!validation.valid) {
    out.error = validation.error;
    return out;   // rien n'a ete touche : ni cfg actif, ni controleurs, ni flash
  }
  out.valid = true;

  // --- 2. Decision de reboot, AVANT toute ecriture ---
  // Le predicat est pur : il ne touche aucun actionneur.
  out.restartRequired = validation.restartRequired ||
                        InstrumentManager::configChangeRequiresRestart(active, candidate);

  // --- 3. Persistance du candidat (flash d'abord) ---
  out.saved = save ? save(candidate) : false;
  if (!out.saved) {
    // Sauvegarde impossible : on n'active RIEN. L'appareil continue exactement
    // sur la configuration precedemment persistee, controleurs inclus.
    out.error = "storage_failed";
    return out;
  }

  // --- 4. Changement necessitant une re-init hardware ---
  // La nouvelle configuration est sauvegardee mais PAS activee : les controleurs
  // continuent de lire la configuration qui correspond au hardware initialise.
  // L'appelant met les actionneurs en securite puis declenche un reboot controle ;
  // c'est au redemarrage que la nouvelle configuration devient reellement active.
  if (out.restartRequired) {
    out.applied = false;
    out.activated = false;
    out.reinitialized = "";
    out.warnings = validation.warnings;
    return out;
  }

  // --- 5. COMMIT ATOMIQUE ---
  // Une seule affectation : les controleurs passent directement de l'ancienne
  // configuration complete a la nouvelle configuration complete et validee.
  RuntimeConfig previous = active;
  active = candidate;
  out.activated = true;

  if (instrument) {
    ConfigApplyResult applyResult = instrument->applyRuntimeConfig(previous, active);
    out.applied = applyResult.applied;
    out.reinitialized = applyResult.reinitialized;
    if (applyResult.warnings.length() > 0) {
      if (out.warnings.length() > 0) out.warnings += "; ";
      out.warnings += applyResult.warnings;
    }
  } else {
    out.applied = true;
  }
  return out;
}

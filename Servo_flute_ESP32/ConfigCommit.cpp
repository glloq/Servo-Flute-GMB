#include "ConfigCommit.h"
#include "InstrumentManager.h"

ConfigCommitResult commitCandidateConfig(RuntimeConfig& active,
                                         RuntimeConfig& candidate,
                                         InstrumentManager* instrument,
                                         ConfigSaveFn save,
                                         const ConfigCommitGuard* guard) {
  // PAS d'initialisation par accolades positionnelle ici. La structure porte
  // desormais des initialiseurs de membre par defaut ; en C++11 - le dialecte
  // que le firmware recoit REELLEMENT, voir la note de platformio.ini sur le
  // -std=gnu++11 ajoute apres nos options par le builder Arduino - cela lui
  // retire la qualite d'agregat, et la liste ne compile pas. La construction
  // par defaut applique exactement les memes valeurs, et surtout elle ne se
  // decale pas silencieusement le jour ou un champ est ajoute au milieu.
  ConfigCommitResult out;

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

  // --- 5. COMMIT ATOMIQUE, SOUS VERROU OBLIGATOIRE ---
  // Une seule affectation : les controleurs passent directement de l'ancienne
  // configuration complete a la nouvelle configuration complete et validee.
  // Le verrou (s'il existe) n'entoure QUE cette affectation : un lecteur
  // concurrent n'attend jamais la validation ni l'ecriture flash.
  //
  // La copie de `previous` se fait hors verrou volontairement : cette fonction
  // est le SEUL ecrivain de la configuration active et elle tourne dans la tache
  // loop(), donc aucune ecriture concurrente ne peut dechirer cette lecture. Le
  // verrou protege les LECTEURS (taches AsyncTCP) contre l'ecrivain, pas
  // l'inverse.
  RuntimeConfig previous = active;
  bool locked = true;
  if (guard && guard->lock) locked = guard->lock(guard->ctx);
  if (!locked) {
    // VERROU EXPIRE = une autre tache est en train de lire ou d'ecrire la
    // configuration active EN CE MOMENT. Ecrire ici lui ferait observer une
    // structure de plusieurs kilo-octets a moitie recopiee : c'est exactement
    // ce que le verrou existe pour empecher. On n'ecrit donc RIEN.
    //
    // CE QUI EST PERDU, ET CE QUI LE REMPLACE : le code precedent ecrivait
    // quand meme, pour ne pas laisser l'appareil tourner sur une configuration
    // differente de celle qui est en flash. Cette coherence RAM/flash reste un
    // vrai besoin - mais elle ne justifie pas de corrompre un lecteur. Elle est
    // desormais retablie par le REDEMARRAGE CONTROLE : le candidat est deja
    // persiste, le reboot le recharge avec l'initialisation hardware
    // correspondante, et RAM et flash se retrouvent d'accord. `restartRequired`
    // est le signal que l'appelant lit deja pour programmer ce reboot (mise en
    // securite des actionneurs comprise).
    //
    // unlock() n'est PAS appele : on ne relache pas un verrou qu'on n'a pas
    // pris (rendre un mutex FreeRTOS qu'on ne detient pas corrompt son
    // compteur, et la protection disparaitrait silencieusement pour tous).
    out.applied = false;
    out.activated = false;
    out.reinitialized = "";
    out.restartRequired = true;
    if (out.warnings.length() > 0) out.warnings += "; ";
    out.warnings += "config_lock_timeout";
    return out;
  }
  active = candidate;
  if (guard && guard->unlock) guard->unlock(guard->ctx);
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

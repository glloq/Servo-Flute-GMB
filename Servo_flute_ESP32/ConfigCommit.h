/***********************************************************************************************
 * ConfigCommit - Commit TRANSACTIONNEL de la configuration runtime
 *
 * PROBLEME RESOLU
 * ---------------
 * POST /api/config modifiait directement le `cfg` global champ par champ, PUIS
 * validait. Entre les deux, loop() (sequenceur, controleurs d'air, pompes)
 * lisait un etat intermediaire : demi-configuration ou, pire, une valeur invalide
 * qui n'aurait jamais du etre appliquee. Une sauvegarde ratee laissait en plus
 * l'appareil sur une configuration appliquee mais non persistee.
 *
 * MODELE TRANSACTIONNEL
 * ---------------------
 *   RuntimeConfig candidate = cfg;      <- copie
 *   appliquer le JSON sur `candidate`   <- aucun effet visible
 *   normaliser + valider integralement  <- aucun effet visible
 *   determiner restartRequired
 *   sauvegarder `candidate`             <- flash d'abord
 *   commit atomique vers la config active  <- UNE seule affectation
 *
 * Aucun etat de configuration intermediaire n'est donc jamais visible par les
 * controleurs. En cas d'echec (validation ou sauvegarde), ni la configuration
 * active ni les controleurs ne bougent.
 *
 * Les changements qui exigent une re-init hardware ne sont PAS actives : la
 * nouvelle configuration est sauvegardee, l'ancienne reste active jusqu'au
 * reboot controle (actionneurs mis en securite par l'appelant).
 *
 * Ce module est volontairement pur (pas de JSON, pas de LittleFS, pas de reseau)
 * pour etre testable sur hote.
 ***********************************************************************************************/
#ifndef CONFIG_COMMIT_H
#define CONFIG_COMMIT_H

#include <Arduino.h>
#include "ConfigStorage.h"

class InstrumentManager;

// `saved` et `activated` sont DEUX choses differentes : la flash et la RAM ne
// sont pas la meme ressource et ne sont pas ecrites au meme moment. Un appelant
// qui ne lit que `saved` croira active un candidat qui ne l'est pas.
//   saved && activated   -> flash et RAM d'accord, rien a faire
//   saved && !activated  -> flash EN AVANCE sur la RAM (re-init hardware
//                           necessaire, ou verrou refuse) : `restartRequired`
//                           est pose, l'appelant doit declencher le reboot
//                           controle qui les reconcilie
//   !saved               -> rien n'a bouge nulle part
struct ConfigCommitResult {
  bool valid = false;             // le candidat a passe la validation complete
  bool corrected = false;         // des valeurs ont ete normalisees
  // Un redemarrage controle est necessaire pour que la RAM rejoigne la flash :
  // soit parce que le changement exige une re-init hardware, soit parce que le
  // verrou de configuration a expire et que l'activation a donc ete refusee.
  bool restartRequired = false;
  bool saved = false;             // le candidat a ete persiste
  bool applied = false;           // la configuration active a ete remplacee ET appliquee
  // `active` a REELLEMENT ete remplace (=> GMB peut bouger). Jamais vrai sans
  // que le verrou ait ete acquis.
  bool activated = false;
  String error;
  String warnings;
  String reinitialized;
};

// Fonction de persistance injectable : ConfigStorage::saveFrom en production,
// un espion en test (pour exercer le chemin "sauvegarde impossible").
typedef bool (*ConfigSaveFn)(const RuntimeConfig& config);

// Verrou OPTIONNEL pris uniquement autour de l'affectation atomique de la
// configuration active, jamais autour de la validation ni de l'ecriture flash.
// Cette affectation recopie ~5 Ko : ce n'est pas atomique vis-a-vis d'un autre
// coeur, et un constructeur de reponse HTTP qui parcourait `cfg` pendant le
// commit pouvait observer une structure a moitie recopiee. Le lecteur prend le
// meme verrou ; le temps de garde du commit se limite a une copie de structure.
//
// `lock` renvoie false en cas d'expiration. L'ACTIVATION EST ALORS REFUSEE : le
// verrou n'expire que si une autre tache tient la configuration active, donc
// ecrire malgre tout livrerait a cette tache la structure a moitie recopiee que
// le verrou est cense interdire. Un avertissement rend le probleme visible sans
// l'empecher : c'est le contraire de ce que fait un verrou.
//   -> `saved` peut valoir true (la flash a bien ete ecrite : elle n'est pas
//      protegee par ce verrou et n'est pas la meme ressource),
//   -> `activated` et `applied` valent false, la configuration active est
//      INCHANGEE, et `warnings` porte "config_lock_timeout",
//   -> `restartRequired` est pose : c'est le redemarrage controle, et non une
//      ecriture hors verrou, qui remet RAM et flash en coherence.
// unlock() n'est pas appele quand lock() a echoue : on ne relache pas un verrou
// qu'on n'a pas pris.
//
// ORDRE VOLONTAIRE : la flash est ecrite AVANT la prise du verrou, jamais
// pendant. Tenir ce verrou pendant une ecriture LittleFS (des centaines de
// millisecondes) ferait attendre la tache web ET loop() derriere un effacement
// de secteur. La divergence RAM/flash qui peut en resulter est bornee, signalee
// et reparable ; un blocage de loop() sur l'instrument ne l'est pas.
struct ConfigCommitGuard {
  bool (*lock)(void* ctx);
  void (*unlock)(void* ctx);
  void* ctx;
};

// Valide `candidate`, le persiste, puis - et seulement en cas de succes complet -
// le rend actif en remplacant `active` en une seule affectation.
// `candidate` est normalise sur place (il peut donc etre corrige).
// `instrument` peut etre nullptr (pas de hardware : seule la config est commitee).
ConfigCommitResult commitCandidateConfig(RuntimeConfig& active,
                                         RuntimeConfig& candidate,
                                         InstrumentManager* instrument,
                                         ConfigSaveFn save,
                                         const ConfigCommitGuard* guard = nullptr);

#endif

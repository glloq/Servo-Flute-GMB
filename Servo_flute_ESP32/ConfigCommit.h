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

struct ConfigCommitResult {
  bool valid;             // le candidat a passe la validation complete
  bool corrected;         // des valeurs ont ete normalisees
  bool restartRequired;   // re-init hardware necessaire (donc pas encore active)
  bool saved;             // le candidat a ete persiste
  bool applied;           // la configuration active a ete remplacee ET appliquee
  bool activated;         // la configuration active a change (=> GMB peut bouger)
  String error;
  String warnings;
  String reinitialized;
};

// Fonction de persistance injectable : ConfigStorage::saveFrom en production,
// un espion en test (pour exercer le chemin "sauvegarde impossible").
typedef bool (*ConfigSaveFn)(const RuntimeConfig& config);

// Verrou OPTIONNEL pris uniquement autour de l'affectation atomique
// `active = candidate`, jamais autour de la validation ni de l'ecriture flash.
// `active = candidate` recopie ~5 Ko : ce n'est pas atomique vis-a-vis d'un
// autre coeur, et un constructeur de reponse HTTP qui parcourait `cfg` pendant
// le commit pouvait observer une structure a moitie recopiee. Le lecteur prend
// le meme verrou ; le temps de garde du commit se limite a une copie de
// structure. `lock` renvoie false en cas d'expiration : le commit se poursuit
// quand meme (le candidat est deja persiste - le refuser laisserait l'appareil
// tourner sur une configuration differente de celle en flash) et le signale par
// l'avertissement "config_lock_timeout".
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

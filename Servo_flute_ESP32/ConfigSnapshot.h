/***********************************************************************************************
 * ConfigSnapshot - Lecture COHERENTE de la configuration active
 *
 * POURQUOI CE FICHIER EXISTE
 * --------------------------
 * `RuntimeConfig` fait ~5 Ko. Le commit l'active par UNE affectation
 * (`cfg = candidat`), mais une affectation de 5 Ko n'est pas atomique vis-a-vis
 * d'un autre coeur : c'est une boucle de copie. Un lecteur qui parcourt `cfg`
 * pendant cette copie voit une structure MI-ANCIENNE MI-NOUVELLE.
 *
 * Le defaut reel corrige ici : `handleApiConfigFinalize()` faisait
 *     RuntimeConfig* candidatePtr = new (std::nothrow) RuntimeConfig(cfg);
 * depuis la tache AsyncTCP, SANS prendre `_cfgMutex`, pendant que loop() pouvait
 * etre en train de remplacer `cfg`. Le candidat sur lequel toute la requete
 * POST /api/config etait ensuite construite pouvait donc melanger deux
 * configurations - par exemple `numNotes` de la nouvelle avec `notes[]` de
 * l'ancienne - et ce melange etait ensuite VALIDE, SAUVEGARDE et ACTIVE.
 * C'est exactement ce que le verrou du commit existe pour empecher.
 *
 * POURQUOI LE VERROU EST INJECTE
 * ------------------------------
 * Meme motif que `ConfigPersist.{h,cpp}` (operations de fichier injectees) et
 * que `ConfigCommitGuard` : ce module reste PUR. Il ne connait ni FreeRTOS, ni
 * `_cfgMutex`, ni WebConfigurator. `WebConfigurator` fournit l'adaptateur vers
 * ses `lockConfig()` / `unlockConfig()` existants ; les tests hote fournissent
 * un faux verrou qui sait REFUSER a volonte et qui COMPTE ses prises et ses
 * relachements. Sans cette injection, la seule facon de verifier "le verrou est
 * pris, et relache exactement une fois" serait de relire le code.
 *
 * CE QUE CE MODULE NE PROUVE PAS
 * ------------------------------
 * Un test hote est mono-tache : il prouve le CONTRAT (refus -> rien de copie ;
 * accord -> copie complete, verrou relache une fois), pas l'absence de course.
 ***********************************************************************************************/
#ifndef CONFIG_SNAPSHOT_H
#define CONFIG_SNAPSHOT_H

#include <stddef.h>

#include "ConfigStorage.h"   // RuntimeConfig

// Verrou injecte. Pointeurs de fonction plutot qu'une classe abstraite : pas de
// vtable, pas d'allocation, une poignee d'octets sur la pile - on est sur ESP32.
// `ctx` est passe tel quel (le WebConfigurator s'y met lui-meme ; les tests y
// mettent leur compteur).
//
// `lock` DOIT rendre false quand le verrou n'a pas ete acquis. C'est la seule
// chose qui permet au lecteur HTTP de repondre `config_busy` / 503 au lieu de
// servir une configuration incoherente.
struct ConfigLockOps {
  bool (*lock)(void* ctx);
  void (*unlock)(void* ctx);
  void* ctx;
};

// Copie coherente de `src` dans `dst`, sous verrou.
//
//   - jeu d'operations incomplet (pointeur nul) -> false, `dst` INTACT ;
//   - verrou REFUSE                             -> false, `dst` INTACT ;
//   - succes                                    -> true, `dst` identique a
//     `src` bit a bit, verrou relache EXACTEMENT une fois.
//
// L'invariant qui compte est le deuxieme : un verrou refuse ne doit laisser
// AUCUNE copie partielle derriere lui. L'appelant qui recoit false n'a rien a
// nettoyer et n'a surtout rien de valide a lire dans `dst`.
bool snapshotConfig(const ConfigLockOps& ops, const RuntimeConfig& src, RuntimeConfig& dst);

// Meme discipline pour une plage d'octets quelconque : utile quand une reponse
// n'a besoin que d'un champ mais qu'une valeur a moitie remplacee y serait
// VISIBLE (tableau, structure imbriquee).
bool snapshotConfigBytes(const ConfigLockOps& ops, const void* src, void* dst, size_t n);

// Copie d'une chaine de la configuration, sous verrou, TOUJOURS terminee.
//
// `cfg.wifiSsid` est un `char[33]`. Le remplacer est une copie d'octets : un
// lecteur concurrent peut en voir la moitie, et rien ne garantit alors qu'un
// '\0' se trouve encore dans la plage. Le serialiseur JSON lirait au-dela du
// tableau. Cette fonction borne la copie a `dstSize - 1` octets et pose le '\0'
// elle-meme, donc le pire cas devient une chaine tronquee - jamais une lecture
// hors limites.
//
// `dst` est laisse INTACT si le verrou est refuse (meme regle que ci-dessus).
// Un `dstSize` nul n'a nulle part ou poser le '\0' : la fonction rend false
// sans rien ecrire et sans prendre le verrou.
//
// PRECONDITION : `dstSize` ne doit pas depasser la taille du tableau SOURCE.
// La lecture est bornee par `dstSize`, pas par le '\0' - c'est voulu, puisque
// l'absence de '\0' est justement le cas qu'on protege. Au point d'appel on
// dimensionne donc `dst` avec `sizeof(cfg.<champ>)`.
bool snapshotConfigString(const ConfigLockOps& ops, const char* src, char* dst, size_t dstSize);

#endif

/***********************************************************************************************
 * ConfigPersist - Remplacement atomique d'un fichier de configuration
 *
 * La sequence de renommages qui remplace /config.json est ici, SEULE et PURE :
 * pas de LittleFS, pas d'ArduinoJson, les operations de systeme de fichiers sont
 * INJECTEES. ConfigStorage.cpp branche les vraies (LittleFS), les tests hote
 * branchent un faux systeme de fichiers en memoire qui sait echouer a volonte.
 *
 * POURQUOI CE FICHIER EXISTE
 * --------------------------
 * La sequence precedente etait :
 *     remove(config); if (!rename(tmp, config)) { remove(tmp); return false; }
 * Un rename rate detruisait les DEUX copies : la bonne configuration venait
 * d'etre supprimee, et la seule copie valide restante (le .tmp, deja ecrit ET
 * relu) etait supprimee juste apres. L'instrument redemarrait sans configuration,
 * donc sur des valeurs d'usine sans rapport avec le cablage reel. La recuperation
 * de .tmp au demarrage ne servait a rien dans ce cas precis : le .tmp n'existait
 * plus.
 *
 * INVARIANT TENU ICI : a chaque instant de la sequence, au moins un des trois
 * chemins (final, .tmp, .bak) contient une configuration complete et lisible, et
 * configRecoverOnBoot() sait la remettre en place au demarrage suivant.
 ***********************************************************************************************/
#ifndef CONFIG_PERSIST_H
#define CONFIG_PERSIST_H

// Operations de systeme de fichiers injectees. Pointeurs de fonction plutot
// qu'une classe abstraite : pas de vtable, pas d'allocation, une poignee d'octets
// sur la pile - on est sur ESP32.
// `ctx` est passe tel quel a chaque operation (nullptr pour LittleFS, qui est un
// singleton global ; le faux systeme de fichiers des tests y met son etat).
struct FsRenameOps {
  bool (*exists)(void* ctx, const char* path);
  bool (*remove)(void* ctx, const char* path);
  bool (*rename)(void* ctx, const char* from, const char* to);
  void* ctx;
};

// Remplace finalPath par le contenu de tmpPath en preservant une copie
// recuperable a chaque instant.
//
// Sequence en trois temps :
//   1. le .bak eventuellement reste d'une passe precedente est libere - mais
//      SEULEMENT si la configuration finale est la, car sans elle ce .bak est la
//      derniere copie connue bonne ;
//   2. la configuration courante est DEPLACEE vers bakPath (jamais supprimee) ;
//   3. le .tmp devient la configuration ; le .bak n'est efface qu'apres.
//
// Retourne true seulement si finalPath porte desormais le contenu de tmpPath.
// En cas d'echec, renvoie false SANS jamais laisser l'etat sans configuration
// lisible : soit finalPath est intact ou restaure, soit bakPath/tmpPath portent
// encore une copie que configRecoverOnBoot() promouvra au demarrage.
//
// tmpPath doit exister et contenir un document DEJA verifie : cette fonction ne
// lit aucun contenu, elle ne fait que deplacer des noms.
bool configAtomicReplace(const FsRenameOps& ops, const char* tmpPath,
                         const char* finalPath, const char* bakPath);

// Chemin de DEMARRAGE : remet le systeme de fichiers dans un etat propre apres
// une sauvegarde interrompue.
//   - finalPath present : les residus (.tmp, .bak) sont perimes, ils sont
//     effaces ;
//   - finalPath absent : promotion du .bak s'il existe, sinon du .tmp.
// Retourne true si une configuration est en place a finalPath en sortie, false
// s'il n'y a rien a recuperer (premier demarrage reel, ou systeme de fichiers
// hors service - dans ce cas la fonction se contente d'echouer, sans boucler).
//
// POURQUOI LE .BAK PASSE AVANT LE .TMP
// ------------------------------------
// L'ordre inverse - .tmp d'abord - transformait une sauvegarde RATEE en
// sauvegarde reussie, un redemarrage plus tard. La sequence :
//     live = OLD, tmp = NEW, pas de bak
//     OLD -> bak   OK
//     tmp -> live  ECHEC
//     bak -> live  ECHEC     => configAtomicReplace() rend FALSE
// laisse exactement tmp = NEW et bak = OLD sans configuration finale.
// L'utilisateur a recu une ERREUR d'enregistrement ; au demarrage suivant le
// .tmp etait pourtant promu et NEW devenait la configuration active. Or NEW peut
// decrire un tout autre cablage (canaux PCA, broches de pompe, mode d'air) que
// celui qui est reellement monte : c'est precisement ce que le refus du
// formatage automatique de LittleFS protege ailleurs dans ce firmware.
//
// La presence d'un .bak signifie qu'une configuration a DEJA ete COMMITEE :
// configAtomicReplace() ne cree le .bak qu'en deplacant une configuration finale
// existante. Elle prime donc toujours. Le .tmp n'est promu que s'il n'y a PAS de
// .bak - c'est-a-dire un tout PREMIER enregistrement interrompu, ou le .tmp est
// la seule chose qui existe et ou le refuser laisserait la machine vierge.
//
// Si le .bak est la mais refuse de bouger, la fonction rend false SANS promouvoir
// le .tmp a sa place : mieux vaut demarrer en mode recovery (interface web
// entierement disponible, actionneurs interdits) que piloter du materiel avec une
// configuration dont l'enregistrement a ete annonce comme echoue. Rien n'est
// efface : le demarrage suivant retentera la meme promotion.
bool configRecoverOnBoot(const FsRenameOps& ops, const char* tmpPath,
                         const char* finalPath, const char* bakPath);

// Chemin RESET USINE : efface la configuration persistee ET ses residus.
//
// L'ORDRE EST LA CORRECTION. La sequence precedente supprimait la configuration
// finale EN PREMIER, puis les residus, puis rendait false si un residu avait
// resiste. L'utilisateur recevait donc une erreur ET avait quand meme perdu sa
// configuration - le pire des deux mondes, et un etat dont il ne peut rien faire :
// ni la configuration d'avant, ni une machine vierge.
//
// Ici les residus partent D'ABORD. S'ils ne partent pas, la fonction abandonne
// AVANT d'avoir touche la configuration finale : l'erreur rendue est alors une
// erreur sans degat, l'instrument redemarre exactement comme avant. La
// configuration finale n'est supprimee qu'en DERNIER, quand plus rien ne peut
// faire echouer la suite.
//
// Retourne true seulement si l'etat demande est REELLEMENT atteint : aucun des
// trois chemins n'existe plus. Invariant tenu : jamais "configuration detruite
// ET false".
bool configFactoryErase(const FsRenameOps& ops, const char* tmpPath,
                        const char* finalPath, const char* bakPath);

#endif

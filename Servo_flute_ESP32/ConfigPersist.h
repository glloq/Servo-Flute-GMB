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
//   - finalPath absent : promotion du .tmp (contenu ecrit et verifie le plus
//     recent), a defaut du .bak (configuration precedente).
// Retourne true si une configuration est en place a finalPath en sortie, false
// s'il n'y a rien a recuperer (premier demarrage reel, ou systeme de fichiers
// hors service - dans ce cas la fonction se contente d'echouer, sans boucler).
bool configRecoverOnBoot(const FsRenameOps& ops, const char* tmpPath,
                         const char* finalPath, const char* bakPath);

#endif

/***********************************************************************************************
 * FileTransaction - Remplacement d'un fichier quelconque sans jamais le perdre
 *
 * Meme discipline que ConfigPersist.{h,cpp} (lire ce fichier d'abord : c'est le
 * modele) et meme raison d'etre : la sequence de renommages est ICI, SEULE et
 * PURE. Pas de LittleFS, pas d'Arduino, pas d'allocation - les operations de
 * systeme de fichiers sont INJECTEES. L'appelant embarque branche les vraies,
 * les tests hote branchent un faux systeme de fichiers en memoire qui sait
 * echouer a volonte.
 *
 * POURQUOI CE FICHIER EXISTE
 * --------------------------
 * L'installation d'un fichier MIDI fraichement televerse (WEBOP_MIDI_FINALIZE)
 * faisait :
 *     LittleFS.remove(destPath);
 *     if (!LittleFS.rename(srcPath, destPath)) { ... repli par copie ... }
 * L'ancien fichier etait DETRUIT avant que le nouveau ne soit en place. Si le
 * rename ET le repli par copie echouaient - carte pleine, secteur fatigue,
 * coupure - il ne restait ni l'ancien ni le nouveau : l'utilisateur perdait le
 * morceau qui marchait pour un televersement qui n'a pas abouti.
 *
 * INVARIANT TENU ICI : a chaque instant de la sequence, une version utilisable
 * de `destPath` existe - a `destPath`, ou a `bakPath`. fileTxRecover() sait la
 * remettre en place au demarrage suivant.
 *
 * DIFFERENCE ASSUMEE AVEC ConfigPersist
 * -------------------------------------
 * configRecoverOnBoot() promeut le .tmp en priorite, parce qu'il a ete ecrit
 * ET RELU avant que le remplacement ne commence. Ici la source est un
 * televersement : rien ne dit qu'il est complet, et le Contrat 3 ne donne
 * d'ailleurs pas son chemin a fileTxRecover(). La recuperation ne connait donc
 * que dest et bak, et promeut l'ANCIEN fichier - le seul dont on sait qu'il
 * jouait.
 ***********************************************************************************************/
#ifndef FILE_TRANSACTION_H
#define FILE_TRANSACTION_H

// Operations de systeme de fichiers injectees. Pointeurs de fonction plutot
// qu'une classe abstraite : pas de vtable, pas d'allocation, une poignee
// d'octets sur la pile - on est sur ESP32.
// `ctx` est passe tel quel a chaque operation (nullptr pour LittleFS, qui est un
// singleton global ; le faux systeme de fichiers des tests y met son etat).
//
// `copy` est la parce que plusieurs versions de LittleFS sur ESP32 ne renomment
// pas de facon fiable - le code d'origine portait deja un repli par copie
// manuelle. Il doit LAISSER la source en place et rendre true seulement si la
// destination porte bien le contenu complet. Il peut valoir nullptr : le repli
// est alors simplement indisponible, et la sequence reste sure (elle echoue
// proprement au lieu de detruire quoi que ce soit).
struct FileTxOps {
  bool (*exists)(void* ctx, const char* path);
  bool (*remove)(void* ctx, const char* path);
  bool (*rename)(void* ctx, const char* from, const char* to);
  bool (*copy)  (void* ctx, const char* from, const char* to);
  void* ctx;
};

// Installe `srcPath` a `destPath` sans jamais laisser `destPath` absent quand il
// existait.
//
// Sequence :
//   1. si `destPath` existe, le `bakPath` eventuellement reste d'une passe
//      precedente est libere - mais SEULEMENT dans ce cas, car sans `destPath`
//      ce `bakPath` est la derniere copie connue bonne (voir plus bas) ;
//   2. `destPath` est DEPLACE vers `bakPath` (jamais supprime) ; si le rename
//      echoue, repli par copie puis suppression, dans cet ordre ;
//   3. `srcPath` devient `destPath` (rename, a defaut copie) ;
//   4. si l'etape 3 echoue, `bakPath` est REMIS a `destPath` ;
//   5. `bakPath` n'est efface qu'apres un succes confirme.
//
// Retourne true seulement si `destPath` porte desormais le contenu de
// `srcPath`. En cas d'echec, retourne false SANS jamais laisser l'utilisateur
// sans fichier jouable : soit `destPath` est intact ou restaure, soit `bakPath`
// le porte encore et fileTxRecover() le promouvra au demarrage.
//
// LE PIEGE A NE PAS REPRODUIRE : une NOUVELLE tentative apres un echec qui
// s'est arrete entre les etapes 2 et 4 trouve `destPath` ABSENT et `bakPath`
// present. Ce `bakPath` est alors la seule copie de l'ancien fichier. L'etape 1
// ne s'execute donc que si `destPath` existe.
bool fileTxInstall(const FileTxOps& ops, const char* srcPath,
                   const char* destPath, const char* bakPath);

// Chemin de DEMARRAGE : nettoie/repare les residus d'une installation coupee.
//   - `destPath` present : le `bakPath` est perime (l'installation est allee au
//     bout, ou a ete annulee proprement), il est efface ;
//   - `destPath` absent : le `bakPath` est promu - c'est l'ancien fichier, le
//     seul dont on sait qu'il etait jouable.
// Retourne true si un fichier utilisable est en place a `destPath` en sortie,
// false s'il n'y a rien a recuperer (aucun fichier n'a jamais ete installe) ou
// si le systeme de fichiers refuse toute mutation - dans ce cas la fonction se
// contente d'echouer, sans boucler.
bool fileTxRecover(const FileTxOps& ops, const char* destPath,
                   const char* bakPath);

#endif

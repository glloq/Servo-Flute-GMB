#include "FileTransaction.h"

namespace {

// Un jeu d'operations incomplet (pointeur nul) ferait planter le firmware au
// premier appel. On refuse franchement : l'appelant verra un echec
// d'installation, jamais un reboot. `copy` fait exception - il est OPTIONNEL,
// c'est un repli, et son absence ne peut que faire echouer proprement.
inline bool opsUsable(const FileTxOps& ops) {
  return ops.exists != nullptr && ops.remove != nullptr && ops.rename != nullptr;
}

inline bool pathsUsable(const char* a, const char* b) {
  return a != nullptr && b != nullptr;
}

inline bool pathsUsable(const char* a, const char* b, const char* c) {
  return pathsUsable(a, b) && c != nullptr;
}

// Deplacement sur : rename d'abord, repli par copie ensuite pour les versions de
// LittleFS qui ne renomment pas de facon fiable.
//
// L'ORDRE compte : on ne supprime la source qu'APRES avoir CONSTATE que la
// destination existe. Copier puis supprimer sans verifier serait exactement le
// defaut d'origine deguise - une copie ratee suivie d'une suppression reussie
// perd le fichier.
//
// Retourne true si la destination porte le contenu ET que la source est partie.
// Si la copie a reussi mais que la suppression de la source echoue, retourne
// false : les DEUX chemins portent alors le meme contenu, donc rien n'est
// perdu, mais l'appelant ne doit pas croire la source liberee.
bool moveFile(const FileTxOps& ops, const char* from, const char* to) {
  if (ops.rename(ops.ctx, from, to)) return true;
  if (ops.copy == nullptr) return false;
  if (!ops.copy(ops.ctx, from, to)) return false;
  if (!ops.exists(ops.ctx, to)) return false;   // copie annoncee mais absente
  ops.remove(ops.ctx, from);
  return !ops.exists(ops.ctx, from);
}

}  // namespace

bool fileTxInstall(const FileTxOps& ops, const char* srcPath,
                   const char* destPath, const char* bakPath) {
  if (!opsUsable(ops) || !pathsUsable(srcPath, destPath, bakPath)) return false;

  // Rien a installer : ne rien detruire pour autant.
  if (!ops.exists(ops.ctx, srcPath)) return false;

  const bool hadDest = ops.exists(ops.ctx, destPath);

  if (hadDest) {
    // 1. Liberer l'emplacement de sauvegarde. Le bak eventuel est redondant
    //    PUISQUE le fichier courant est la ; et un bak qui refuse de disparaitre
    //    ferait echouer le deplacement de l'etape 2 sur un systeme de fichiers
    //    qui n'ecrase pas a l'arrivee. On s'arrete AVANT d'avoir touche quoi que
    //    ce soit : le fichier courant est encore en place.
    if (ops.exists(ops.ctx, bakPath)) {
      ops.remove(ops.ctx, bakPath);
      if (ops.exists(ops.ctx, bakPath)) return false;
    }

    // 2. Mettre le fichier courant DE COTE (et non a la poubelle).
    if (!moveFile(ops, destPath, bakPath)) {
      // Echec sans consequence : soit le fichier courant n'a pas bouge, soit il
      // a ete copie sans pouvoir etre supprime - dans les deux cas il est
      // toujours lisible a destPath, et la source attend toujours. Un demarrage
      // maintenant garde l'ancien fichier (fileTxRecover efface alors le bak
      // redondant).
      return false;
    }
  }
  // Pas de fichier a destPath : soit c'est la premiere installation, soit une
  // installation precedente a ete coupee entre les etapes 2 et 4. Dans ce
  // second cas, un bak present est la DERNIERE copie de l'ancien fichier - on
  // n'y touche pas avant que le nouveau ne soit en place. C'est le piege que
  // l'etape 1 aurait reproduit si elle s'executait inconditionnellement.

  // 3. Promouvoir le nouveau contenu. Le repli par copie est admis : le fichier
  //    source est un televersement deja ecrit, le dupliquer ne perd rien.
  bool installed = ops.rename(ops.ctx, srcPath, destPath);
  if (!installed && ops.copy != nullptr) {
    installed = ops.copy(ops.ctx, srcPath, destPath) && ops.exists(ops.ctx, destPath);
  }

  if (!installed) {
    // C'est ICI que la sequence d'origine perdait tout : l'ancien fichier avait
    // deja ete supprime. Il est maintenant dans le bak - on le remet en place.
    if (hadDest) {
      if (!ops.rename(ops.ctx, bakPath, destPath) && ops.copy != nullptr) {
        // Repli : une copie suffit a rendre le fichier jouable. On garde le bak
        // dans ce cas ; il est redondant, pas dangereux, et la prochaine
        // installation (ou fileTxRecover) l'effacera - alors que le supprimer
        // ici sur la foi d'une copie non relue serait un pari.
        ops.copy(ops.ctx, bakPath, destPath);
      }
      // Si meme cette restauration echoue, on ne supprime RIEN : le bak porte
      // l'ancien fichier et fileTxRecover() le promouvra au prochain demarrage.
    }
    return false;
  }

  // 4. Le nouveau fichier est en place : la sauvegarde ne sert plus. Un bak qui
  //    survit n'est pas une erreur (il sera efface au demarrage suivant, ou par
  //    l'installation suivante), donc ces suppressions sont "au mieux".
  if (ops.exists(ops.ctx, bakPath)) ops.remove(ops.ctx, bakPath);
  // Idem pour la source : elle n'existe plus si le rename a abouti, mais le
  // repli par copie l'a laissee en place.
  if (ops.exists(ops.ctx, srcPath)) ops.remove(ops.ctx, srcPath);
  return true;
}

bool fileTxRecover(const FileTxOps& ops, const char* destPath,
                   const char* bakPath) {
  if (!opsUsable(ops) || !pathsUsable(destPath, bakPath)) return false;

  if (ops.exists(ops.ctx, destPath)) {
    // Fichier en place : un bak residuel vient d'une installation interrompue
    // APRES coup, ou d'une restauration par copie. Le garder ferait promouvoir
    // un jour un fichier que l'utilisateur a deja remplace.
    if (ops.exists(ops.ctx, bakPath)) ops.remove(ops.ctx, bakPath);
    return true;
  }

  // Fichier absent : la coupure est tombee entre la mise de cote et la
  // promotion. Le bak est l'ancien fichier, le seul dont on sait qu'il jouait.
  if (ops.exists(ops.ctx, bakPath)) {
    if (ops.rename(ops.ctx, bakPath, destPath)) return true;
    if (ops.copy != nullptr && ops.copy(ops.ctx, bakPath, destPath) &&
        ops.exists(ops.ctx, destPath)) {
      ops.remove(ops.ctx, bakPath);   // au mieux : le doublon n'est pas nuisible
      return true;
    }
    return false;
  }

  // Ni fichier, ni copie : aucune installation n'a jamais abouti (ou tout a ete
  // efface volontairement). Rien a recuperer, et surtout rien a creer.
  return false;
}

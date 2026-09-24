#include "ConfigPersist.h"

namespace {

// Un jeu d'operations incomplet (pointeur nul) ferait planter le firmware au
// premier appel. On refuse franchement : l'appelant verra un echec de
// persistance, jamais un reboot.
inline bool opsUsable(const FsRenameOps& ops) {
  return ops.exists != nullptr && ops.remove != nullptr && ops.rename != nullptr;
}

inline bool pathsUsable(const char* tmpPath, const char* finalPath, const char* bakPath) {
  return tmpPath != nullptr && finalPath != nullptr && bakPath != nullptr;
}

}  // namespace

bool configAtomicReplace(const FsRenameOps& ops, const char* tmpPath,
                         const char* finalPath, const char* bakPath) {
  if (!opsUsable(ops) || !pathsUsable(tmpPath, finalPath, bakPath)) return false;

  // Rien a promouvoir : ne rien detruire pour autant.
  if (!ops.exists(ops.ctx, tmpPath)) return false;

  const bool hadFinal = ops.exists(ops.ctx, finalPath);

  if (hadFinal) {
    // 1. Liberer l'emplacement de sauvegarde. Le .bak eventuel est redondant
    //    puisque la configuration courante est la ; et un .bak qui refuse de
    //    disparaitre ferait echouer le deplacement de l'etape 2 sur un systeme de
    //    fichiers qui n'ecrase pas a l'arrivee. On s'arrete AVANT d'avoir touche
    //    quoi que ce soit : la configuration courante est encore en place.
    if (ops.exists(ops.ctx, bakPath)) {
      ops.remove(ops.ctx, bakPath);
      if (ops.exists(ops.ctx, bakPath)) return false;
    }

    // 2. Mettre la configuration courante DE COTE (et non a la poubelle).
    if (!ops.rename(ops.ctx, finalPath, bakPath)) {
      // Echec sans consequence : la configuration courante n'a pas bouge et le
      // nouveau contenu attend toujours dans le .tmp. Un demarrage maintenant
      // garde l'ancienne configuration (le .tmp est perime des lors que le final
      // existe).
      return false;
    }
  }
  // Pas de configuration finale : un remplacement precedent a ete interrompu. Un
  // .bak present est alors la DERNIERE copie connue bonne - on n'y touche pas
  // avant que la nouvelle configuration ne soit en place.

  // 3. Promouvoir le nouveau contenu.
  if (!ops.rename(ops.ctx, tmpPath, finalPath)) {
    // C'est ICI que l'ancienne sequence perdait tout. Le .bak est la : on le
    // remet en place. Si meme cette restauration echoue, on ne supprime RIEN :
    // le .bak (et le .tmp) restent, et configRecoverOnBoot() les promouvra au
    // prochain demarrage.
    if (hadFinal) ops.rename(ops.ctx, bakPath, finalPath);
    return false;
  }

  // 4. La nouvelle configuration est en place : la sauvegarde ne sert plus. Un
  //    .bak qui survit n'est pas une erreur (il sera efface au demarrage suivant,
  //    ou par la sauvegarde suivante), donc la suppression est "au mieux".
  if (ops.exists(ops.ctx, bakPath)) ops.remove(ops.ctx, bakPath);
  return true;
}

bool configRecoverOnBoot(const FsRenameOps& ops, const char* tmpPath,
                         const char* finalPath, const char* bakPath) {
  if (!opsUsable(ops) || !pathsUsable(tmpPath, finalPath, bakPath)) return false;

  if (ops.exists(ops.ctx, finalPath)) {
    // Configuration en place : tout residu vient d'une sauvegarde interrompue
    // APRES coup ou d'un .tmp perime. Les garder ferait promouvoir un jour une
    // configuration que l'utilisateur a deja remplacee.
    if (ops.exists(ops.ctx, tmpPath)) ops.remove(ops.ctx, tmpPath);
    if (ops.exists(ops.ctx, bakPath)) ops.remove(ops.ctx, bakPath);
    return true;
  }

  // Configuration absente : la coupure est tombee au milieu d'un remplacement.
  // Le .tmp est le contenu le plus recent, deja ecrit ET relu avant que le
  // remplacement ne commence : il passe en premier.
  if (ops.exists(ops.ctx, tmpPath) && ops.rename(ops.ctx, tmpPath, finalPath)) {
    if (ops.exists(ops.ctx, bakPath)) ops.remove(ops.ctx, bakPath);
    return true;
  }

  // A defaut, la configuration precedente mise de cote a l'etape 2.
  if (ops.exists(ops.ctx, bakPath) && ops.rename(ops.ctx, bakPath, finalPath)) {
    return true;
  }

  // Ni configuration, ni copie : premier demarrage reel (ou reset usine), ou
  // systeme de fichiers hors service. Dans les deux cas on rend la main.
  return false;
}

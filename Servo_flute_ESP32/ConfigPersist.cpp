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

// L'effacement n'a besoin ni de rename ni d'un jeu complet : exiger un pointeur
// dont on ne se sert pas ferait echouer un reset usine parfaitement realisable.
inline bool eraseOpsUsable(const FsRenameOps& ops) {
  return ops.exists != nullptr && ops.remove != nullptr;
}

// "Ce chemin n'existe plus" - la seule question qui compte pour un effacement.
// Un remove() qui rend false sur un fichier deja absent n'est pas un echec, et
// un remove() qui rend true sans rien supprimer ne doit pas passer pour un
// succes : c'est exists() qui tranche, jamais la valeur rendue par remove().
inline bool ensureAbsent(const FsRenameOps& ops, const char* path) {
  if (!ops.exists(ops.ctx, path)) return true;
  ops.remove(ops.ctx, path);
  return !ops.exists(ops.ctx, path);
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
  //
  // LE .BAK PASSE AVANT LE .TMP (voir la demonstration complete dans
  // ConfigPersist.h). Un .bak n'existe que parce qu'une configuration finale a
  // ete deplacee la : c'est la derniere configuration COMMITEE. Un .tmp n'est
  // qu'un candidat, et s'il est encore la alors que la configuration finale a
  // disparu, c'est que sa promotion a ECHOUE - l'utilisateur a vu l'erreur. Le
  // promouvoir ici rendrait cette sauvegarde ratee effective un redemarrage plus
  // tard, en silence.
  if (ops.exists(ops.ctx, bakPath)) {
    if (!ops.rename(ops.ctx, bakPath, finalPath)) {
      // On NE se rabat PAS sur le .tmp. Le .bak est la configuration a remettre ;
      // s'il resiste, l'etat honnete est "pas de configuration" (mode recovery,
      // interface web disponible), pas "la configuration que l'utilisateur a vu
      // echouer". Rien n'est efface : le demarrage suivant retentera.
      return false;
    }
    // La configuration commitee est de retour : le candidat qui n'a pas su
    // prendre sa place est perime.
    if (ops.exists(ops.ctx, tmpPath)) ops.remove(ops.ctx, tmpPath);
    return true;
  }

  // Pas de .bak : aucune configuration n'a jamais ete commitee sur ce systeme de
  // fichiers, ou le reset usine est passe par la. Le .tmp est alors la SEULE
  // chose qui existe - typiquement un tout premier enregistrement interrompu.
  // Le refuser laisserait une machine vierge alors qu'un contenu deja ecrit ET
  // relu attend ; on le promeut.
  if (ops.exists(ops.ctx, tmpPath) && ops.rename(ops.ctx, tmpPath, finalPath)) {
    return true;
  }

  // Ni configuration, ni copie : premier demarrage reel (ou reset usine), ou
  // systeme de fichiers hors service. Dans les deux cas on rend la main.
  return false;
}

bool configFactoryErase(const FsRenameOps& ops, const char* tmpPath,
                        const char* finalPath, const char* bakPath) {
  if (!eraseOpsUsable(ops) || !pathsUsable(tmpPath, finalPath, bakPath)) return false;

  // 1. LES RESIDUS D'ABORD. Ils comptent autant que le fichier lui-meme :
  //    configRecoverOnBoot() promeut un .bak (ou, a defaut, un .tmp) quand la
  //    configuration finale manque. En laisser un ressusciterait au demarrage
  //    suivant la configuration que l'utilisateur vient d'effacer, alors que
  //    isFirstBoot() aurait annonce une machine vierge.
  //
  // 2. UN RESIDU QUI RESISTE ARRETE TOUT, configuration finale INTACTE. C'est la
  //    difference avec la sequence precedente, qui supprimait la configuration en
  //    premier : elle pouvait rendre false apres l'avoir detruite. L'utilisateur
  //    recevait une erreur et avait quand meme perdu sa configuration.
  if (!ensureAbsent(ops, tmpPath)) return false;
  if (!ensureAbsent(ops, bakPath)) return false;

  // 3. La configuration finale EN DERNIER : plus rien apres elle ne peut echouer.
  if (!ensureAbsent(ops, finalPath)) return false;

  // 4. L'etat demande est-il REELLEMENT atteint ? On ne rend true que sur
  //    constat, jamais sur l'absence d'erreur : un systeme de fichiers qui
  //    acquitte sans supprimer existe (c'est pour lui que ensureAbsent relit).
  return !ops.exists(ops.ctx, finalPath) &&
         !ops.exists(ops.ctx, tmpPath) &&
         !ops.exists(ops.ctx, bakPath);
}

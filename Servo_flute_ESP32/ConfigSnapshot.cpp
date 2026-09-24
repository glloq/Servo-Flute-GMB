#include "ConfigSnapshot.h"

#include <string.h>

namespace {

// Un jeu d'operations incomplet ferait planter le firmware au premier appel. On
// refuse franchement : l'appelant HTTP verra `config_busy`, jamais un reboot.
// Meme discipline que ConfigPersist::opsUsable().
inline bool lockOpsUsable(const ConfigLockOps& ops) {
  return ops.lock != nullptr && ops.unlock != nullptr;
}

}  // namespace

bool snapshotConfigBytes(const ConfigLockOps& ops, const void* src, void* dst, size_t n) {
  if (!lockOpsUsable(ops)) return false;
  if (src == nullptr || dst == nullptr) return false;
  // Copie de zero octet : rien a serialiser, et prendre le verrou pour cela
  // ferait attendre un commit sans aucune raison.
  if (n == 0) return true;
  // Source et destination confondues : la copie serait un chevauchement (memcpy
  // est alors indefini) pour un resultat nul. On ne prend pas le verrou.
  if (src == dst) return true;

  // ORDRE ESSENTIEL : rien n'est ecrit dans `dst` AVANT que le verrou ne soit
  // acquis. C'est ce qui garantit "verrou refuse => destination intacte" -
  // l'appelant qui recoit false n'a aucune copie partielle a demeler.
  if (!ops.lock(ops.ctx)) return false;
  memcpy(dst, src, n);
  ops.unlock(ops.ctx);
  return true;
}

bool snapshotConfig(const ConfigLockOps& ops, const RuntimeConfig& src, RuntimeConfig& dst) {
  return snapshotConfigBytes(ops, &src, &dst, sizeof(RuntimeConfig));
}

bool snapshotConfigString(const ConfigLockOps& ops, const char* src, char* dst, size_t dstSize) {
  if (!lockOpsUsable(ops)) return false;
  if (src == nullptr || dst == nullptr) return false;
  // Sans place pour le '\0' final il n'y a pas de chaine terminee possible :
  // on echoue plutot que de rendre un tampon que l'appelant croirait sur.
  if (dstSize == 0) return false;
  if (src == dst) return true;

  if (!ops.lock(ops.ctx)) return false;
  // strncpy ne termine PAS quand la source est trop longue ; on copie donc au
  // plus dstSize - 1 octets et on pose le terminateur nous-memes. Le pire cas
  // devient une chaine tronquee - jamais une lecture hors du tableau source,
  // qui est precisement ce qu'une copie a moitie faite peut produire.
  size_t i = 0;
  for (; i + 1 < dstSize; i++) {
    const char c = src[i];
    if (c == '\0') break;
    dst[i] = c;
  }
  dst[i] = '\0';
  ops.unlock(ops.ctx);
  return true;
}

/***********************************************************************************************
 * FormatGuard - la sequence sure autour d'un formatage LittleFS
 *
 * LE PROBLEME
 * -----------
 * `LittleFS.format()` efface ~1,9 Mo. L'operation est BLOQUANTE et se deroule
 * en grande partie cache d'instructions desactive : `loop()` ne tourne pas, donc
 * `esp_task_wdt_reset()` n'est pas appele, et le chien de garde de tache -
 * arme a WATCHDOG_TIMEOUT_MS = 4000 ms avec `trigger_panic = true` - redemarre
 * la carte EN PLEIN FORMATAGE.
 *
 * C'est le chemin de recuperation documente pour une carte vierge ou un
 * systeme de fichiers corrompu : precisement celui qu'on emprunte au premier
 * bring-up, et precisement celui qui doit marcher du premier coup.
 *
 * LA SEQUENCE
 * -----------
 *     MISE EN SECURITE DU MATERIEL      <- actionneurs au repos, /OE HIGH
 *       -> suspension du chien de garde <- desinscription de la tache courante
 *          -> LittleFS.format()
 *       -> restauration du chien de garde
 *     -> remontage, puis redemarrage controle (hors de ce module)
 *
 * TROIS REGLES, et ce sont elles que les tests verrouillent :
 *
 *  1. le materiel est mis en securite AVANT toute modification du chien de
 *     garde, et si cette mise en securite echoue, RIEN d'autre n'a lieu - ni
 *     suspension, ni formatage ;
 *  2. si la suspension echoue, le formatage N'A PAS LIEU. Formater quand meme
 *     reviendrait a declencher sciemment le redemarrage qu'on cherche a
 *     eviter, au milieu d'un effacement de flash ;
 *  3. il n'existe AUCUN chemin de retour qui laisse le chien de garde
 *     suspendu. Succes ou echec du formatage, la restauration a lieu - et si
 *     elle echoue elle-meme, l'appelant l'apprend au lieu de le supposer.
 *
 * CE QUE CE MODULE N'EST PAS
 * --------------------------
 * Il n'allonge pas le plafond du chien de garde et ne le desactive pas
 * globalement. Il retire la TACHE COURANTE de sa surveillance, le temps d'une
 * operation volontaire, authentifiee et bornee - puis l'y remet. Les autres
 * taches surveillees, s'il y en a, ne sont pas touchees.
 *
 * Les primitives sont INJECTEES : c'est ce qui rend la sequence executable sur
 * hote. Les appels ESP-IDF reels vivent dans TaskWatchdog.{h,cpp} et ne sont
 * compiles que par le build ESP32.
 ***********************************************************************************************/
#ifndef FORMAT_GUARD_H
#define FORMAT_GUARD_H

#include <stdint.h>

enum FormatGuardResult : uint8_t {
  FMT_OK = 0,
  FMT_BAD_OPS,             // primitive manquante : erreur de programmation, refus
  FMT_UNSAFE_HARDWARE,     // mise en securite impossible : rien n'a ete tente
  FMT_WDT_SUSPEND_FAILED,  // chien de garde non suspendable : formatage refuse
  FMT_FORMAT_FAILED,       // formatage echoue ; le chien de garde EST restaure
};

struct FormatGuardOps {
  // Met les actionneurs au repos et coupe l'alimentation des servos (/OE HIGH).
  // False = on n'est pas en mesure de garantir un materiel inerte.
  bool (*safeHardware)(void*);
  // Retire la tache courante de la surveillance du chien de garde.
  bool (*suspendWatchdog)(void*);
  // L'y remet.
  bool (*restoreWatchdog)(void*);
  // L'operation bloquante elle-meme (LittleFS.end/format/begin).
  bool (*format)(void*);
  void* ctx;
};

struct FormatGuardOutcome {
  FormatGuardResult result;
  bool watchdogSuspended;  // la suspension a-t-elle reellement eu lieu ?
  bool watchdogRestored;   // ... et la restauration ? Doit etre vraie des que
                           // watchdogSuspended l'est, sauf si la restauration
                           // a elle-meme echoue - auquel cas on le DIT.
};

FormatGuardOutcome formatGuarded(const FormatGuardOps& ops);

// Code d'erreur destine au client HTTP. Chaine stable, sans allocation.
const char* formatGuardErrorCode(FormatGuardResult r);

#endif

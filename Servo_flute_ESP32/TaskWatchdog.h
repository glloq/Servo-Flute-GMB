/***********************************************************************************************
 * TaskWatchdog - les deux seuls appels ESP-IDF necessaires pour retirer, puis
 * remettre, la TACHE COURANTE sous la surveillance du chien de garde.
 *
 * POURQUOI CE FICHIER EXISTE
 * --------------------------
 * Pour qu'aucun appel `esp_task_wdt_*` ne se disperse dans `WebConfigurator`.
 * La sequence qui les ordonne vit dans FormatGuard (pur, testable sur hote) ;
 * ici il n'y a que l'adaptation a la plateforme.
 *
 * API CHOISIE, ET POURQUOI CELLE-LA
 * ---------------------------------
 * `esp_task_wdt_delete()` / `esp_task_wdt_add()` existent a l'identique sur
 * IDF 4.x (arduino-esp32 2.x, donc espressif32@6.10.0 et 6.11.0) et sur IDF
 * 5.x. C'est le mecanisme officiel de desinscription d'une tache, et il ne
 * demande aucune garde de version - contrairement a `esp_task_wdt_init()`,
 * dont la signature change entre les deux et que le sketch entoure deja d'un
 * `#if ESP_IDF_VERSION`.
 *
 * CE QU'IL NE FAUT PAS FAIRE A LA PLACE, et pourquoi :
 *  - allonger WATCHDOG_TIMEOUT_MS : cela affaiblirait la surveillance de TOUTE
 *    la vie du firmware pour une operation qui dure quelques secondes une fois
 *    dans la vie d'une carte ;
 *  - `esp_task_wdt_deinit()` : cela desarme le chien de garde pour toutes les
 *    taches, pas seulement la notre ;
 *  - ne rien faire et "esperer" : c'est l'etat documente comme limitation
 *    connue jusqu'ici, et il fait redemarrer la carte au milieu d'un effacement
 *    de flash.
 *
 * `ESP_ERR_INVALID_STATE` est traite comme un succes dans les deux sens : il
 * signifie "la tache n'etait pas inscrite" (a la suppression) ou "elle l'etait
 * deja" (a l'ajout). Dans les deux cas l'etat voulu est atteint.
 *
 * CE FICHIER N'EST COMPILE QUE PAR LE BUILD ESP32. Aucun build hote ne le voit :
 * il n'existe pas de `esp_task_wdt.h` sur hote, et un faux en-tete ne
 * prouverait rien. C'est donc la compilation ESP32 de la CI - les deux
 * versions de plateforme - qui atteste que ces appels existent et sont bien
 * typés.
 ***********************************************************************************************/
#ifndef TASK_WATCHDOG_H
#define TASK_WATCHDOG_H

// Retire la tache appelante de la surveillance du chien de garde de tache.
// True = elle n'est plus surveillee (y compris si elle ne l'etait deja pas).
bool taskWatchdogSuspendCurrent();

// L'y remet. True = elle est surveillee (y compris si elle l'etait deja).
bool taskWatchdogResumeCurrent();

#endif

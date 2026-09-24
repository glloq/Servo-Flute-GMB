#include "TaskWatchdog.h"

#include <esp_task_wdt.h>

bool taskWatchdogSuspendCurrent() {
  const esp_err_t err = esp_task_wdt_delete(NULL);
  // ESP_ERR_INVALID_STATE = la tache n'etait pas inscrite. L'etat voulu - non
  // surveillee - est atteint, donc c'est un succes.
  return err == ESP_OK || err == ESP_ERR_INVALID_STATE;
}

bool taskWatchdogResumeCurrent() {
  const esp_err_t err = esp_task_wdt_add(NULL);
  // Meme raisonnement en miroir : deja inscrite est l'etat voulu.
  return err == ESP_OK || err == ESP_ERR_INVALID_STATE;
}

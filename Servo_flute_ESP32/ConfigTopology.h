/***********************************************************************************************
 * ConfigTopology - Quels champs de configuration decrivent le MATERIEL
 *
 * Un champ de RuntimeConfig appartient a l'une de deux familles :
 *
 *   - les champs MUSICAUX ou d'interface (volume CC, courbe de souffle, couleur,
 *     seuils, angles, temporisations). Les controleurs les relisent a chaque note :
 *     les changer a chaud suffit, et c'est ce que fait applyRuntimeConfig() ;
 *
 *   - les champs de TOPOLOGIE : ceux qu'un begin() applique UNE FOIS au demarrage
 *     et que plus rien ne relit ensuite. pinMode(), routage PCA9685/I2C,
 *     initialisation de capteur, configuration d'UART. Les changer a chaud ne
 *     change RIEN au materiel : le firmware croit piloter la nouvelle
 *     configuration pendant que les broches, les canaux et l'UART sont encore
 *     cables selon l'ancienne. C'est le pire des etats - une description qui ne
 *     correspond plus au montage - et c'est exactement le danger que
 *     bootConfigMayDriveActuators() decrit dans ConfigStorage.h.
 *
 * Ce fichier porte la TABLE de la seconde famille, et rien d'autre. Il est PUR :
 * aucun include Arduino/LittleFS/ArduinoJson au-dela de ConfigStorage.h, donc il
 * se compile et s'execute sur l'hote, contrairement a ConfigStorage.cpp.
 *
 * POURQUOI UNE TABLE ET PAS UNE EXPRESSION BOOLEENNE
 * --------------------------------------------------
 * La version precedente etait une longue disjonction dans
 * InstrumentManager::configChangeRequiresRestart(). Trois champs y manquaient -
 * endstopPin, endstopActiveHigh et hallPin - alors que PressureController::begin()
 * fait pinMode() sur les deux premiers et pinMode()+analogRead() sur le troisieme.
 * Un oubli dans une disjonction ne se voit pas : il n'y a rien a compter, rien a
 * parcourir, rien a nommer. Une table se parcourt, ses entrees portent un nom et
 * la raison de leur presence, et un test peut exiger que la table et lui-meme se
 * connaissent MUTUELLEMENT - c'est ce que fait test_fin_storage.cpp. Un champ
 * ajoute a la table sans test echoue ; un champ retire de la table echoue aussi.
 ***********************************************************************************************/
#ifndef CONFIG_TOPOLOGY_H
#define CONFIG_TOPOLOGY_H

#include <stddef.h>

#include "ConfigStorage.h"

// Un champ (ou un groupe de champs indissociables, comme un tableau de broches)
// dont le changement exige une re-initialisation materielle.
//
// Agregat SANS initialiseur de membre par defaut : le firmware est compile en
// gnu++11 (voir le commentaire de platformio.ini), ou un agregat qui en porte
// cesse d'etre un agregat et ne peut plus etre initialise par liste. La table de
// ConfigTopology.cpp est justement une liste.
struct ConfigTopologyField {
  const char* name;        // nom du champ, tel qu'il s'ecrit dans RuntimeConfig
  bool (*changed)(const RuntimeConfig& oldCfg, const RuntimeConfig& newCfg);
  const char* appliedBy;   // le begin()/init qui l'applique, et lui seul
};

// La table, parcourable. Exposee pour que les tests verifient la COUVERTURE et
// pas seulement quelques cas, et pour que les diagnostics puissent nommer le
// champ qui impose le redemarrage plutot que de dire "quelque chose a change".
size_t configTopologyFieldCount();
const ConfigTopologyField& configTopologyFieldAt(size_t index);

// Vrai si passer de `oldCfg` a `newCfg` change quoi que ce soit que seul un
// begin()/init hardware peut reappliquer : pinMode, routage PCA/I2C,
// initialisation capteur, configuration UART.
bool configTopologyRequiresRestart(const RuntimeConfig& oldCfg,
                                   const RuntimeConfig& newCfg);

// Le nom du PREMIER champ de topologie qui differe, ou nullptr si aucun. Rend
// un message de diagnostic utile ("numPumps") la ou l'interface web n'affichait
// qu'une phrase generique.
const char* configTopologyChangedField(const RuntimeConfig& oldCfg,
                                       const RuntimeConfig& newCfg);

#endif

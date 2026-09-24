#include "ConfigTopology.h"

namespace {

// Un comparateur par entree de table. Le macro evite vingt fonctions copiees a
// la main, ou une faute de frappe (`a.hallPin != b.endstopPin`) serait invisible.
#define TOPO_SCALAR(member)                                                    \
  bool topoChanged_##member(const RuntimeConfig& a, const RuntimeConfig& b) {  \
    return a.member != b.member;                                               \
  }

TOPO_SCALAR(numFingers)
TOPO_SCALAR(airflowPcaChannel)
TOPO_SCALAR(angleServoEnabled)
TOPO_SCALAR(angleServoPcaChannel)
TOPO_SCALAR(airMode)
TOPO_SCALAR(valveType)
TOPO_SCALAR(valveServoPcaChannel)
TOPO_SCALAR(solenoidPin)
TOPO_SCALAR(fanPin)
TOPO_SCALAR(numPumps)
TOPO_SCALAR(motorType)
TOPO_SCALAR(sensorType)
TOPO_SCALAR(endstopPin)
TOPO_SCALAR(endstopActiveHigh)
TOPO_SCALAR(hallPin)
TOPO_SCALAR(serialMidiEnabled)
TOPO_SCALAR(serialMidiRxPin)

#undef TOPO_SCALAR

// Les deux tableaux sont parcourus sur TOUTE leur longueur, pas seulement sur
// les [0..numFingers-1] / [0..numPumps-1] actifs. C'est volontairement plus
// strict que necessaire : une entree inactive aujourd'hui devient active des que
// le compte augmente, et le compte peut changer dans la MEME sauvegarde. Comparer
// uniquement la partie active laisserait passer le couple "numFingers 3 -> 5 +
// fingers[4].pcaChannel modifie" sur le seul changement de compte, donc avec la
// bonne conclusion par accident. Ici la conclusion est juste par construction.
bool topoChanged_fingerPcaChannels(const RuntimeConfig& a, const RuntimeConfig& b) {
  for (uint8_t i = 0; i < MAX_FINGER_SERVOS; i++) {
    if (a.fingers[i].pcaChannel != b.fingers[i].pcaChannel) return true;
  }
  return false;
}

bool topoChanged_pumpPins(const RuntimeConfig& a, const RuntimeConfig& b) {
  for (uint8_t i = 0; i < MAX_PUMPS; i++) {
    if (a.pumpPins[i] != b.pumpPins[i]) return true;
  }
  return false;
}

// =============================================================================
// LA TABLE
//
// Une ligne = un champ que SEUL un begin() applique. La troisieme colonne dit
// lequel : c'est elle qui permet de verifier une ligne sans relire tout le
// firmware, et c'est elle qui manquera le jour ou quelqu'un voudra ajouter une
// ligne sans savoir pourquoi.
//
// CE QUI N'EST PAS ICI, ET POURQUOI - la moitie utile de la table :
//
//   numNotes, notes[], fingers[].closedAngle/direction/isThumbHole/halfPercent,
//   fingerAngleOpen, halfHolePercent, embouchure, midiChannel, servoAirflow*,
//   servoAngle*, vibrato*, cc*Default, cc2*, solenoidPwm*, solenoidActivationTimeMs,
//   airAttack*, airVelocityResponse, servoToSolenoidDelayMs,
//   minNoteIntervalForValveCloseMs, minNoteDurationMs, timeUnpower,
//   valveServoCloseAngle/OpenAngle, fanMinPwm/MaxPwm/IdlePercent/IdleTimeoutMs/
//   DefaultPercent/MaxNotePercent/FollowAirflow, pumpMinPwm[], pumpMaxPwm[],
//   pumpCascadeThreshold, pumpStaggerMs, pumpDirectIdlePercent/MaxPercent,
//   pumpFollowAirflow, reservoirTargetPercent, bangbangHysteresis, sensorTargetMm,
//   sensorMinMm, sensorMaxMm, pidKp, pidKi, endstopPumpOn, hallThresholdLow/High,
//   showAirSystem, resFormat, midiStorageLimitKb, hideCalibration, hideAir,
//   instrumentColor, kbdMode
//     -> relus a chaque note, a chaque boucle de regulation ou a chaque rendu de
//        page. Les inscrire ici ferait redemarrer l'instrument au moindre reglage
//        musical : un changement de volume couperait le son pendant le reboot.
//
//   reservoirAutoStart -> lu une seule fois, dans beginSafe(), mais c'est une
//        POLITIQUE de demarrage, pas un cablage. L'utilisateur obtient le meme
//        effet immediatement depuis l'interface (ACMD_PUMP_TARGET) ; rien de
//        materiel n'est a reconfigurer.
//
//   wifiSsid, wifiPassword -> appliques a chaud par la bascule reseau explicite
//        de WebConfigurator (startSTA(), qui fait deja panic + demontage). Les
//        mettre ici ferait redemarrer l'instrument a chaque saisie de mot de passe
//        wifi, et par-dessus le marche AVANT que la bascule ait eu lieu.
//
//   deviceName -> nom BLE (BleMidiHandler::begin) et nom mDNS
//        (WifiMidiHandler). Seul un begin() le reapplique, donc il ne changera
//        effectivement qu'au prochain demarrage - mais c'est un LIBELLE, pas une
//        topologie : rien de physique n'est mal pilote entre-temps. Rebooter un
//        instrument pour le renommer couterait plus que d'attendre. Signale ici
//        pour que le choix soit visible et non oublie.
// =============================================================================
const ConfigTopologyField kFields[] = {
  // --- Comptes et routage PCA9685 -------------------------------------------
  // requiresSecondPca() decide au demarrage si la 2e carte PCA9685 est sondee et
  // initialisee ; son absence est alors une erreur fatale de boot. Tout ce qui
  // alimente cette decision est donc de la topologie.
  { "numFingers",           &topoChanged_numFingers,
    "InstrumentManager::requiresSecondPca() + FingerController::begin()" },
  { "fingers[].pcaChannel", &topoChanged_fingerPcaChannels,
    "InstrumentManager::requiresSecondPca() (canal PCA d'un servo de doigt)" },
  { "airflowPcaChannel",    &topoChanged_airflowPcaChannel,
    "InstrumentManager::requiresSecondPca() (canal PCA du servo de souffle)" },
  { "angleServoEnabled",    &topoChanged_angleServoEnabled,
    "InstrumentManager::requiresSecondPca() + initializeSafeOutputs()" },
  { "angleServoPcaChannel", &topoChanged_angleServoPcaChannel,
    "InstrumentManager::requiresSecondPca() (canal PCA du servo d'angle)" },

  // --- Mode d'air : il choisit QUELS begin() tournent ------------------------
  { "airMode",              &topoChanged_airMode,
    "InstrumentManager::beginSafe() (PressureController::begin / FanController::begin)" },
  { "valveType",            &topoChanged_valveType,
    "AirflowController::begin() pinMode(solenoidPin) + requiresSecondPca()" },
  { "valveServoPcaChannel", &topoChanged_valveServoPcaChannel,
    "InstrumentManager::requiresSecondPca() (canal PCA de la valve servo)" },

  // --- Broches GPIO : pinMode() n'est fait qu'au demarrage -------------------
  { "solenoidPin",          &topoChanged_solenoidPin,
    "AirflowController::begin() / driveConfiguredActuatorPinsInactive() pinMode()" },
  { "fanPin",               &topoChanged_fanPin,
    "FanController::begin() pinMode() + analogWrite()" },
  { "numPumps",             &topoChanged_numPumps,
    "PressureController::begin() pinMode() sur pumpPins[0..numPumps-1]" },
  { "pumpPins[]",           &topoChanged_pumpPins,
    "PressureController::begin() pinMode() (broche de pompe)" },
  { "motorType",            &topoChanged_motorType,
    "PressureController::begin() : digitalWrite (On/Off) ou analogWrite (PWM, qui "
    "attache un canal LEDC a la broche). Basculer a chaud laisse la broche pilotee "
    "par le mauvais peripherique." },

  // --- Capteur de reservoir : initialise une seule fois ----------------------
  // PressureController::begin() recopie cfg.sensorType dans _sensorType ; plus
  // rien ne relit cfg.sensorType ensuite. Et l'initialisation ToF (Model ID,
  // SPAD, tuning) ne se refait pas non plus.
  { "sensorType",           &topoChanged_sensorType,
    "PressureController::begin() (_sensorType latche + TofSensor::begin)" },
  { "endstopPin",           &topoChanged_endstopPin,
    "PressureController::begin() pinMode(endstopPin, ...)" },
  { "endstopActiveHigh",    &topoChanged_endstopActiveHigh,
    "PressureController::begin() : choisit INPUT_PULLUP ou INPUT_PULLDOWN. Le "
    "rappel interne produit le niveau INACTIF ; se tromper fait lire "
    "\"reservoir plein\" en permanence." },
  { "hallPin",              &topoChanged_hallPin,
    "PressureController::begin() pinMode(hallPin, INPUT) + sonde analogRead()" },

  // --- UART MIDI DIN --------------------------------------------------------
  { "serialMidiEnabled",    &topoChanged_serialMidiEnabled,
    "SerialMidiHandler::begin() : Serial2.begin() n'est appele qu'au demarrage" },
  { "serialMidiRxPin",      &topoChanged_serialMidiRxPin,
    "SerialMidiHandler::begin() : Serial2.begin(..., rxPin, -1) mappe l'UART" },
};

const size_t kFieldCount = sizeof(kFields) / sizeof(kFields[0]);

}  // namespace

size_t configTopologyFieldCount() {
  return kFieldCount;
}

const ConfigTopologyField& configTopologyFieldAt(size_t index) {
  // Bornage plutot qu'un acces hors tableau : cette fonction est appelee depuis
  // des tests et des diagnostics, jamais depuis un chemin chaud.
  if (index >= kFieldCount) return kFields[0];
  return kFields[index];
}

const char* configTopologyChangedField(const RuntimeConfig& oldCfg,
                                       const RuntimeConfig& newCfg) {
  for (size_t i = 0; i < kFieldCount; i++) {
    if (kFields[i].changed(oldCfg, newCfg)) return kFields[i].name;
  }
  return nullptr;
}

bool configTopologyRequiresRestart(const RuntimeConfig& oldCfg,
                                   const RuntimeConfig& newCfg) {
  return configTopologyChangedField(oldCfg, newCfg) != nullptr;
}

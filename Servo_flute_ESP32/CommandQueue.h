/***********************************************************************************************
 * CommandQueue - File de commandes actionneurs inter-taches
 *
 * PROBLEME RESOLU
 * ---------------
 * Les callbacks AsyncTCP (HTTP / WebSocket) et NimBLE (connexion / deconnexion
 * BLE) s'executent dans des taches FreeRTOS distinctes de loop(). Quand ils
 * pilotaient directement les controleurs, ils emettaient des transactions I2C
 * vers les PCA9685 et des ecritures GPIO (solenoide, pompes, ventilateur, OE)
 * pendant que loop() faisait de meme : bus I2C entrelace, etat d'actionneur
 * incoherent, et `cfg` modifie champ par champ sous le nez des controleurs.
 *
 * ARCHITECTURE
 * ------------
 *   AsyncTCP / WebSocket / HTTP / NimBLE / MIDI  ->  CommandQueue (verrouillee)
 *                                                  ->  loop() / InstrumentManager::update()
 *                                                  ->  application de la commande
 *
 * Les actionneurs et la configuration active n'ont donc qu'UN SEUL proprietaire :
 * la tache loop(). Rien d'autre ne touche au bus I2C ni aux GPIO d'actionneurs.
 *
 * Le panic (All Sound Off / perte de transport) n'utilise PAS un emplacement de
 * la file : c'est un drapeau dedie, donc il ne peut jamais etre perdu par
 * saturation. Le poser vide aussi la file : aucune commande anterieure au panic
 * n'est appliquee apres lui.
 ***********************************************************************************************/
#ifndef COMMAND_QUEUE_H
#define COMMAND_QUEUE_H

#include <Arduino.h>

enum ActuatorCommandType : uint8_t {
  ACMD_NONE = 0,
  ACMD_NOTE_ON,             // a = note, b = velocity
  ACMD_NOTE_OFF,            // a = note
  ACMD_CONTROL_CHANGE,      // a = cc number, b = cc value
  ACMD_RESET_CONTROLLERS,
  ACMD_TEST_FINGER,         // a = finger index, c = angle
  ACMD_TEST_AIRFLOW_ANGLE,  // c = angle
  ACMD_TEST_ANGLE_SERVO,    // c = angle
  ACMD_AIR_LIVE_PERCENT,    // b = percent
  ACMD_ANGLE_LIVE_PERCENT,  // b = percent
  ACMD_TEST_SOLENOID,       // a = open (0/1)
  ACMD_PUMP_TARGET,         // b = percent
  ACMD_PUMP_SINGLE_TEST,    // a = pump index, b = percent
  ACMD_PUMP_STOP_SINGLE,
  ACMD_PUMP_STOP,
  ACMD_PUMP_ENABLE,         // a = enabled (0/1)
  ACMD_FAN_TARGET,          // b = percent
  ACMD_FAN_STOP,
  ACMD_OPEN_ALL_FINGERS,
  ACMD_ALL_SOUND_OFF,
  ACMD_SET_ACTUATOR_SESSION // a = active (0/1)
};

struct ActuatorCommand {
  uint8_t type;
  uint8_t a;
  uint8_t b;
  uint16_t c;

  ActuatorCommand() : type(ACMD_NONE), a(0), b(0), c(0) {}
  ActuatorCommand(uint8_t t, uint8_t aa = 0, uint8_t bb = 0, uint16_t cc = 0)
    : type(t), a(aa), b(bb), c(cc) {}
};

class CommandQueue {
public:
  explicit CommandQueue(uint8_t capacity);
  ~CommandQueue();

  // Producteur (n'importe quelle tache). Retourne false si la file est pleine ;
  // la commande est alors comptee comme perdue (remontee par les diagnostics).
  bool push(const ActuatorCommand& cmd);

  // Consommateur (tache loop() uniquement).
  bool pop(ActuatorCommand& out);

  // Demande de panic : jamais perdue, vide la file en meme temps pour qu'aucune
  // commande emise avant le panic ne soit appliquee apres lui.
  void requestPanic();
  bool takePanicRequest();
  bool panicPending() const;

  void clear();

  uint8_t count() const;
  uint16_t droppedCount() const;
  void resetDroppedCount();

private:
  ActuatorCommand* _items;
  uint8_t _capacity;
  uint8_t _head;
  uint8_t _tail;
  uint8_t _count;
  uint16_t _dropped;
  bool _panic;
  mutable portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
};

#endif

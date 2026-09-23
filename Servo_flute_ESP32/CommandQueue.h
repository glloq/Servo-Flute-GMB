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
 *
 * DEUX ETAGES, ET C'EST DELIBERE
 * ------------------------------
 * L'anneau (`_items`) est alloue sur le tas ; le panic et les Note Off, eux,
 * vivent dans des champs de l'objet (un booleen et un bitmap de 128 bits). Si
 * l'allocation de l'anneau echoue, la file entre en mode degrade SUR - push()
 * et pop() refusent, count() vaut 0, rien n'est dereference - mais les deux
 * chemins non perdables continuent de fonctionner. Une carte dont le tas est
 * fragmente au point de refuser 24 commandes peut encore s'arreter et relacher
 * ses notes : voir storageAvailable().
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

  // --- Note Off non perdables -------------------------------------------------
  // Un Note Off est deja protege DANS l'EventQueue (enqueue...Forced evince le
  // plus ancien plutot que d'echouer). Mais un Note Off venu du WebSocket
  // traverse d'abord CETTE file, dont le push echoue quand elle est pleine : le
  // relachement n'atteignait alors jamais l'EventQueue et la note restait
  // bloquee, valve et souffle ouverts. Les Note Off sont donc enregistres dans
  // un bitmap de 128 bits plutot que dans l'anneau : ils ne peuvent pas etre
  // perdus, quelle que soit la charge.
  void requestNoteOff(uint8_t note);
  // Retire le plus petit numero de note en attente. Retourne false quand il n'y
  // en a plus.
  bool takePendingNoteOff(uint8_t& note);
  bool hasPendingNoteOff() const;

  void clear();

  uint8_t count() const;
  uint16_t droppedCount() const;
  void resetDroppedCount();

  // Vrai si l'anneau a REELLEMENT ete alloue. Faux = allocation refusee (ou
  // capacite nulle) : push() et pop() refusent proprement, count() vaut 0, et
  // aucun pointeur nul n'est dereference. La panne est aussi visible sans cet
  // accesseur, car chaque push refuse incremente droppedCount(), que les
  // diagnostics web remontent deja.
  //
  // Le panic et les Note Off ne sont PAS concernes : ils ne vivent pas dans
  // l'anneau. Ils restent operationnels, c'est le peu de securite qui doit
  // survivre a un tas mort.
  //
  // Sans verrou : `_items` est fixe par le constructeur et ne change plus
  // jamais ; il n'y a donc rien a serialiser ici.
  bool storageAvailable() const { return _items != nullptr; }

private:
  ActuatorCommand* _items;
  uint8_t _capacity;
  uint8_t _head;
  uint8_t _tail;
  uint8_t _count;
  uint16_t _dropped;
  bool _panic;
  uint32_t _pendingNoteOff[4];   // bitmap 128 notes MIDI
  mutable portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
};

#endif

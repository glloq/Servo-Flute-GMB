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
 * L'anneau (`_items`) est alloue sur le tas ; le panic, les ordres d'ARRET et
 * les Note Off, eux, vivent dans des champs de l'objet (des booleens et des
 * bitmaps). Si l'allocation de l'anneau echoue, la file entre en mode degrade
 * SUR - push() et pop() refusent, count() vaut 0, rien n'est dereference - mais
 * les chemins non perdables continuent de fonctionner. Une carte dont le tas est
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

  // --- Ordres d'ARRET non perdables -------------------------------------------
  // MEME discipline que le panic ci-dessus, et pour la meme raison : ces ordres
  // RETIRENT de l'energie a un actionneur. Les laisser dans l'anneau, c'est
  // accepter qu'un client qui le sature fasse disparaitre l'ordre d'arret -
  // `postCommand()` rendait alors false, valeur que l'appelant web ignore, et la
  // pompe restait alimentee a sa consigne. Un drapeau dedie ne peut pas etre
  // plein : N demandes se COALESCENT en une, jamais en zero.
  //
  // Les variantes qui AJOUTENT de l'energie (ouvrir la valve, reactiver les
  // pompes) restent dans l'anneau ordinaire : perdre une mise en route est sur,
  // perdre un arret ne l'est pas.
  static const uint8_t STOPREQ_PUMPS     = 1 << 0;  // toutes pompes a l'arret
  static const uint8_t STOPREQ_FAN       = 1 << 1;  // ventilateur a l'arret
  static const uint8_t STOPREQ_PUMPS_OFF = 1 << 2;  // pump_enable = false
  static const uint8_t STOPREQ_SOLENOID  = 1 << 3;  // solenoide/valve fermes
  // CONSIGNES A ZERO. Elles retirent de l'energie exactement comme les quatre
  // ci-dessus, et l'interface web ne passe PAS par pump_stop / fan_stop pour
  // ramener un actionneur a zero : ses curseurs envoient "pump_target" /
  // "fan_target" avec v = 0. Sans ces deux bits, cette intention-la restait
  // dans l'anneau ordinaire et se perdait sur saturation.
  //
  // BITS DISTINCTS DE STOPREQ_PUMPS / STOPREQ_FAN, deliberement : une consigne
  // a zero n'est PAS un arret dur. PressureController::stop() annule en plus le
  // test mono-pompe et ecrase le PWM immediatement ; FanController::stop()
  // saute la rampe de descente. Les confondre changerait le comportement
  // observable de l'interface. Le consommateur applique donc exactement la
  // commande d'origine (ACMD_PUMP_TARGET / ACMD_FAN_TARGET avec b = 0).
  static const uint8_t STOPREQ_PUMP_TARGET_ZERO = 1 << 4;  // consigne pompe = 0 %
  static const uint8_t STOPREQ_FAN_TARGET_ZERO  = 1 << 5;  // consigne ventilateur = 0 %

  // Depose un ou plusieurs ordres d'arret. Jamais perdu.
  //
  // Le depot PURGE aussi de l'anneau les commandes qui realimenteraient les
  // memes actionneurs (voir requestStop() dans le .cpp) : le panic resout deja
  // ce probleme en vidant l'anneau EN ENTIER, un arret cible n'en retire que ce
  // qui le concerne. Sans cette purge, une consigne de pompe emise AVANT
  // l'arret serait appliquee APRES lui et relancerait la pompe.
  void requestStop(uint8_t bits);
  // Lit ET efface en une seule section critique (comme takePanicRequest) : une
  // demande deposee par une autre tache apres la prise est conservee pour la
  // passe suivante au lieu d'etre effacee sans avoir ete traitee.
  uint8_t takeStopRequests();
  bool stopRequestsPending() const;

  // Arret d'UNE pompe (test mono-pompe) : bitmap par index, comme les Note Off,
  // pour que deux arrets de pompes differentes ne s'ecrasent pas.
  static const uint8_t COMMAND_QUEUE_MAX_PUMP_STOPS = 8;   // largeur du bitmap
  void requestPumpStopSingle(uint8_t pumpIndex);
  bool takePendingPumpStop(uint8_t& pumpIndex);
  bool hasPendingPumpStop() const;

  // --- Note Off non perdables -------------------------------------------------
  // Un Note Off est deja protege DANS l'EventQueue (enqueue...Forced evince le
  // plus ancien plutot que d'echouer). Mais un Note Off venu du WebSocket
  // traverse d'abord CETTE file, dont le push echoue quand elle est pleine : le
  // relachement n'atteignait alors jamais l'EventQueue et la note restait
  // bloquee, valve et souffle ouverts. Les Note Off sont donc enregistres dans
  // un bitmap de 128 bits plutot que dans l'anneau : ils ne peuvent pas etre
  // perdus, quelle que soit la charge.
  //
  // MAIS le bitmap est un canal SEPARE de l'anneau, donc sans ordre relatif avec
  // lui. Y envoyer SYSTEMATIQUEMENT les Note Off inversait la sequence du cas
  // nominal : "Note Off 60 puis Note On 60" emis entre deux tours de loop()
  // s'appliquait "Note On puis Note Off", et la note finissait muette alors que
  // l'utilisateur l'avait demandee sonnante. Le Note Off passe donc par l'anneau
  // comme tout le monde (pushOrFallback ci-dessous), et le bitmap ne sert plus
  // que de REPLI quand l'anneau refuse - ce qui restaure exactement l'hypothese
  // sur laquelle repose l'ordre d'application de processCommands().
  void requestNoteOff(uint8_t note);
  // Depot d'un ordre NON PERDABLE dans l'anneau : identique a push(), SAUF qu'un
  // refus n'est PAS compte comme une perte. L'appelant a un canal de repli qui
  // ne peut pas echouer, donc rien n'est perdu et le compteur de diagnostic ne
  // doit pas le pretendre - sinon dropped_commands se mettrait a accuser une
  // saturation benigne.
  bool pushOrFallback(const ActuatorCommand& cmd);
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
  // Le panic, les ordres d'arret et les Note Off ne sont PAS concernes : ils ne
  // vivent pas dans l'anneau. Ils restent operationnels, c'est le peu de
  // securite qui doit survivre a un tas mort.
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
  uint8_t _stopRequests;         // OU des STOPREQ_* en attente
  uint8_t _pendingPumpStops;     // bitmap des arrets mono-pompe en attente
  uint32_t _pendingNoteOff[4];   // bitmap 128 notes MIDI
  mutable portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;

  // Compacte l'anneau en retirant ce qui REALIMENTERAIT un actionneur vise par
  // `bits`. `allPumpTests` retire en plus TOUS les tests mono-pompe, quel que
  // soit leur index : l'arret mono-pompe agit lui aussi globalement
  // (PressureController::stopSinglePumpTest() n'a pas d'index), donc laisser en
  // file le test d'une AUTRE pompe le ferait demarrer juste apres l'arret.
  // A n'appeler que sous `_mux` DEJA pris.
  void purgeEnergizingLocked(uint8_t bits, bool allPumpTests);
  // Vrai si `cmd` realimente l'un des actionneurs vises par `bits`.
  static bool commandEnergizes(const ActuatorCommand& cmd, uint8_t bits);
};

#endif

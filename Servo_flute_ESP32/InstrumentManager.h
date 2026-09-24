#ifndef INSTRUMENT_MANAGER_H
#define INSTRUMENT_MANAGER_H

#include <Arduino.h>
#include <Wire.h>   // must precede Adafruit_PWMServoDriver.h (declares TwoWire)
#include <Adafruit_PWMServoDriver.h>
#include "EventQueue.h"
#include "CommandQueue.h"
#include "FingerController.h"
#include "AirflowController.h"
#include "PressureController.h"
#include "FanController.h"
#include "NoteSequencer.h"
#include "CalibrationAirSupply.h"
#include "settings.h"
#include "ConfigStorage.h"

// PHASE 7 : chronometrie acoustique. Declaration AVANCEE - le manager n'en
// detient qu'un pointeur OPTIONNEL qu'il transmet a la chaine d'actionneurs ;
// il n'appelle lui-meme aucune de ses methodes.
class AcousticTiming;

// PHASE 7 (suite) : observateur AUDIO. Meme declaration avancee, meme regle -
// le manager n'en detient qu'un pointeur OPTIONNEL qu'il transmet ; il
// n'appelle lui-meme aucune de ses methodes.
class IAudioSource;

enum HardwareInitStatus {
  HW_INIT_OK,
  HW_PCA0_MISSING,
  HW_PCA1_MISSING,
  HW_I2C_ERROR,
  HW_CONFIG_INVALID
};

struct ConfigApplyResult {
  bool applied;
  bool restartRequired;
  String reinitialized;
  String warnings;
};

/*------------------------------------------------------------------------------
 * BORNES DU TRAVAIL APPLIQUE PAR PASSE D'update()
 *
 * processCommands() drainait la file ENTIEREMENT a chaque passe. Or chaque
 * commande appliquee peut emettre plusieurs transactions I2C vers les PCA9685 :
 * sous saturation (un client WebSocket qui pousse en rafale), une seule passe
 * executait des dizaines d'ecritures d'affilee, le MIDI n'etait plus servi, les
 * notes partaient en retard et le chien de garde pouvait mordre.
 *
 * La borne DIFFERE, elle ne PERD rien : ce qui reste est repris a la passe
 * suivante. L'anneau est FIFO et pop() sert toujours la plus ancienne, donc une
 * file constamment pleine continue d'ecouler ses plus vieilles commandes : pas
 * de famine. Le panic, lui, n'est jamais soumis a la borne (drapeau dedie,
 * consomme en tete de passe).
 *
 * Ces constantes vivent ICI et pas dans settings.h : elles decrivent le rythme
 * interne de l'ordonnanceur de commandes, pas une option d'instrument.
 *----------------------------------------------------------------------------*/

// COMMAND_QUEUE_SIZE vaut 24 : une file pleine s'ecoule en 4 passes de 6 au lieu
// d'une seule rafale de 24, donc le pire cas d'UNE passe est divise par quatre.
// Le cout paye en echange est de 3 tours de loop() sur la derniere commande
// d'une file pleine - et loop() ne contient aucune temporisation (voir
// Servo_flute_ESP32.ino), donc un tour vaut le temps du travail lui-meme, pas
// une periode fixe. Descendre la borne plus bas allongerait ce delai sans
// reduire davantage le pire cas, deja domine par UNE commande (l'ouverture de
// tous les doigts ecrit un canal PCA9685 par doigt).
static const uint8_t INSTRUMENT_MAX_COMMANDS_PER_UPDATE = 6;

// Les Note Off en attente ne vivent pas dans l'anneau mais dans un bitmap de
// 128 bits (voir CommandQueue.h) : ils ne peuvent pas etre perdus. Leur borne
// empeche une rafale de 128 relachements d'enfiler d'un coup plus d'evenements
// que l'EventQueue n'en tient (EVENT_QUEUE_SIZE = 16) : au-dela, l'enfilement
// FORCE evince le plus ancien, donc une rafale non bornee se mangerait
// elle-meme. 8 = la moitie de la file d'evenements, ce qui laisse de la place
// aux Note On deja programmes.
static const uint8_t INSTRUMENT_MAX_NOTE_OFFS_PER_UPDATE = 8;

// Les Note Off en attente sont appliques APRES l'anneau (l'ordre et sa raison
// sont expliques dans processCommands()). Sous saturation PERMANENTE, l'anneau
// n'est jamais vide : sans garde, le relachement attendrait indefiniment et la
// note resterait bloquee, valve et souffle ouverts. Passe ce nombre de passes
// consecutives de report, les relachements passent sans attendre que l'anneau
// soit vide. Deux tours de loop() de retard sur un relachement, contre une note
// bloquee : c'est l'arbitrage deja retenu par le firmware (au pire on perd une
// note, jamais on n'en bloque une).
static const uint8_t INSTRUMENT_MAX_NOTE_OFF_DEFERRAL_PASSES = 2;

class InstrumentManager {
public:
  InstrumentManager();

  void begin();
  bool beginSafe();
  void initializeSafeOutputs();
  // Force chaque GPIO d'actionneur CONFIGURABLE (solenoide, ventilateur, pompes)
  // a son niveau inactif. Appele avant le sondage I2C pour qu'un echec
  // d'initialisation ne laisse aucune sortie en haute impedance.
  void driveConfiguredActuatorPinsInactive();
  void update();

  // Interface MIDI (appelee par BleMidiHandler ou WifiMidiHandler)
  void noteOn(byte midiNote, byte velocity);
  void noteOff(byte midiNote);

  bool isNotePlayable(byte midiNote) const;
  NoteSequencer& getSequencer();

  // Gere les Control Change MIDI
  void handleControlChange(byte ccNumber, byte ccValue);

  // Accesseurs CC
  byte getCCVolume() const { return _ccVolume; }
  byte getCCExpression() const { return _ccExpression; }
  byte getCCModulation() const { return _ccModulation; }
  byte getCCBreath() const { return _ccBreath; }
  byte getCCBrightness() const { return _ccBrightness; }

  void allSoundOff();

  // Nombre de paniques survenues depuis le demarrage. Monotone croissant, il ne
  // redescend jamais et ne se remet pas a zero : un consommateur memorise la
  // derniere valeur vue et compare. Sert a ce qu'un observateur exterieur
  // (le calibrateur) apprenne qu'une panique a eu lieu, quel que soit le chemin
  // qui l'a declenchee - y compris un transport MIDI, qui ne passe par aucun
  // code web.
  //
  // Le defaut qu'il repare : requestCalibrationCancel() n'existe que dans
  // WebConfigurator et n'est appelee que par les chemins WEB. La deconnexion
  // BLE, la deconnexion rtpMIDI, la chute du lien Wi-Fi STA, le timeout Active
  // Sensing du MIDI serie et les CC120/123 recus par n'importe quel transport
  // declenchaient bien le panic, mais l'auto-calibration continuait : elle
  // rouvrait la valve ~740 ms plus tard et la mise en securite etait defaite.
  //
  // uint32_t deborde apres 4 milliards de paniques : sans objet, mais le
  // consommateur compare par INEGALITE, pas par ordre, donc meme un
  // debordement se comporte correctement.
  //
  // Une panique est comptee une seule fois, a l'endroit ou elle est REELLEMENT
  // executee (executePanic), pas a chaque DEMANDE : requestPanic() venue d'une
  // autre tache est coalescee par CommandQueue et ne compte qu'une fois, a la
  // consommation. Tous les incrementations ont lieu sur la tache proprietaire
  // des actionneurs (loop()), celle qui lit aussi ce compteur.
  uint32_t panicCount() const;

  // --- File de commandes inter-taches (voir CommandQueue.h) -------------------
  // Tout appelant qui n'est PAS la tache loop() (callbacks AsyncTCP/WebSocket,
  // callbacks de connexion NimBLE) doit passer par ici : la commande est appliquee
  // plus tard par update(), sur la tache proprietaire des actionneurs.
  // Retourne false si la file est pleine (commande perdue et comptabilisee).
  //
  // EXCEPTION : les commandes qui RETIRENT de l'energie ne passent pas par
  // l'anneau et rendent TOUJOURS true - ACMD_ALL_SOUND_OFF, ACMD_NOTE_OFF,
  // ACMD_PUMP_STOP, ACMD_FAN_STOP, ACMD_PUMP_STOP_SINGLE, ACMD_PUMP_ENABLE avec
  // a == 0, ACMD_TEST_SOLENOID avec a == 0, ACMD_PUMP_TARGET et ACMD_FAN_TARGET
  // avec b == 0, et le CC121. Les variantes qui AJOUTENT de l'energie
  // (ACMD_PUMP_ENABLE a == 1, ACMD_TEST_SOLENOID a == 1, une consigne b > 0)
  // restent ordinaires : perdre une mise en route est sur, perdre un arret ne
  // l'est pas. Voir CommandQueue::requestStop().
  //
  // Une consigne a ZERO n'est PAS convertie en arret dur : elle emprunte un
  // canal imperdable distinct et reste appliquee par la meme commande, afin de
  // ne pas changer le comportement observable (stop() annule en plus le test
  // mono-pompe et saute la rampe du ventilateur).
  bool postCommand(const ActuatorCommand& cmd);
  bool postCommand(uint8_t type, uint8_t a = 0, uint8_t b = 0, uint16_t c = 0);
  // Panic asynchrone : jamais perdu, prioritaire sur toute commande en attente.
  void requestPanic();
  const CommandQueue& commandQueue() const { return _commands; }
  uint16_t droppedCommandCount() const { return _commands.droppedCount(); }
  // Vrai si les DEUX files inter-taches ont obtenu leur stockage au demarrage.
  // Faux = tas trop fragmente a l'initialisation : l'instrument tourne en mode
  // degrade SUR - commandes et evenements refuses au lieu d'un pointeur nul
  // dereference - et les deux chemins non perdables (panic, Note Off) restent
  // operationnels. La file d'evenements n'etant exposee nulle part ailleurs,
  // c'est le seul point d'observation de sa panne : a remonter par les
  // diagnostics web, a cote de dropped_commands.
  bool queuesStorageAvailable() const {
    return _commands.storageAvailable() && _eventQueue.storageAvailable();
  }
  // Vrai si la commande touche physiquement un actionneur : refusee tant que le
  // hardware n'est pas pret (voir isHardwareReady()).
  static bool commandDrivesActuators(uint8_t type);
  // Vrai si la commande peut AJOUTER de l'energie a un actionneur ou le
  // DEPLACER. C'est une question differente de commandDrivesActuators(), et les
  // confondre est precisement le piege de cette garde : une consigne de pompe a
  // zero "pilote un actionneur" (elle ecrit un PWM) tout en ne pouvant qu'en
  // RETIRER de l'energie. Une garde ecrite sur commandDrivesActuators()
  // bloquerait donc le canal imperdable par lequel l'interface coupe une pompe.
  //
  // Sert a la garde de session d'actionneurs : pendant une auto-calibration,
  // seules les commandes pour lesquelles ce predicat est FAUX traversent.
  static bool commandMayEnergizeActuator(const ActuatorCommand& cmd);
  // Panic entry point for loss of a live MIDI transport (BLE/rtpMIDI/Wi-Fi/DIN).
  // A held note whose Note Off can no longer arrive would otherwise keep the
  // valve/airflow/pump/fan energized indefinitely, so route every disconnect to
  // allSoundOff(). Mirrors what MidiFilePlayer already does on stop/pause.
  void handleTransportLost();

  // --- PHASE 7 : observateur de chronometrie acoustique ----------------------
  // OPTIONNEL : nullptr par defaut, et l'instrument doit se comporter STRICTEMENT
  // de la meme facon sans lui. Le pointeur est transmis tel quel au sequenceur et
  // au controleur de souffle, qui NOTIFIENT les quatre instants d'ordre
  // (note commandee, air commande, valve ouverte, note relachee) aux endroits ou
  // ces ordres agissent reellement sur un actionneur.
  //
  // Le flux est A SENS UNIQUE, et ce n'est pas un detail de style : le cahier des
  // charges interdit que le moteur audio puisse maintenir un actionneur active
  // ou perturber sa securite. Aucune decision d'actionneur ne lit la valeur
  // rendue par un hook ni ne teste la presence de l'observateur ; un
  // AcousticTiming absent, fige ou plante ne change donc rien a la mecanique.
  //
  // Appartenance : l'appelant reste proprietaire de l'objet vise et doit le
  // maintenir en vie tant qu'il est pose (passer nullptr pour le retirer).
  void setTimingObserver(AcousticTiming* obs);
  AcousticTiming* timingObserver() const { return _timingObserver; }

  // --- PHASE 7 : observateur AUDIO -------------------------------------------
  // OPTIONNEL, nullptr par defaut, MEME discipline que ci-dessus : le pointeur
  // est transmis tel quel, le flux est a SENS UNIQUE, et l'instrument doit se
  // comporter STRICTEMENT de la meme facon avec ou sans lui.
  //
  // Un SEUL destinataire, le sequenceur, et c'est delibere : ce que cet
  // observateur recoit, c'est "la note a change", et les seuls instants ou une
  // note change sont les deux bornes que le sequenceur possede. Le controleur
  // de souffle, lui, n'a que des instants d'ORDRE intermediaires (consigne
  // d'air, valve ouverte) : lui faire signaler un changement de note serait
  // signaler une note neuve au milieu d'une note tenue.
  //
  // INDEPENDANT de setTimingObserver() : poser l'un n'impose pas l'autre.
  // Appartenance : l'appelant reste proprietaire de l'objet vise et doit le
  // maintenir en vie tant qu'il est pose (passer nullptr pour le retirer).
  void setAudioObserver(IAudioSource* obs);
  IAudioSource* audioObserver() const { return _audioObserver; }

  void resetAllControllers();
  void powerOnServos();
  void ensureServosPowered();
  void registerActuatorActivity();

  // --- Demandes differees a drapeau dedie (voir _requestMux) ------------------
  // MEME motif que CommandQueue::requestPanic() / takePanicRequest() : la prise
  // LIT ET EFFACE sous une seule section critique. Une demande deposee par une
  // autre tache apres la prise est donc conservee pour la passe suivante au lieu
  // d'etre effacee sans avoir ete traitee.
  //
  // `take...()` est reserve a la tache proprietaire des actionneurs (loop()) :
  // c'est une CONSOMMATION. Les observer sans consommer se fait avec les
  // predicats `...Pending()`.
  bool takePowerOnRequest();
  bool takeResetControllersRequest();
  bool powerOnRequestPending() const;
  bool resetControllersRequestPending() const;
  // While true, the idle power-down is inhibited and the servos are kept powered
  // (used by the auto-calibrator / range finder, which drive actuators and read
  // audio outside the MIDI sequencer that managePower() watches).
  void setActuatorSessionActive(bool active);
  bool isActuatorSessionActive() const { return _actuatorSessionActive; }

  // Ecriture PWM multi-PCA9685 : route vers la bonne carte (canal 0-15 = carte 0, 16-31 = carte 1)
  void setPWM(uint8_t channel, uint16_t on, uint16_t off);

  // Calibration : acces direct aux controleurs
  FingerController& getFingerCtrl() { return _fingerCtrl; }
  AirflowController& getAirflowCtrl() { return _airflowCtrl; }
  PressureController& getPressureCtrl() { return _pressureCtrl; }
  FanController& getFanCtrl() { return _fanCtrl; }
  // Air-source bridge for the auto-calibrator (drives pump/fan/reservoir per air mode).
  ICalibrationAirSupply& getCalibrationAirSupply() { return _calAirSupply; }
  HardwareInitStatus hardwareInitStatus() const { return _hardwareInitStatus; }
  // True only after a fully successful safe init. When false (PCA missing / config
  // invalid), all actuator paths (noteOn/noteOff/CC/setPWM/power/update) are no-ops
  // so a failed board can never be driven.
  bool isHardwareReady() const { return _hardwareInitStatus == HW_INIT_OK; }
  bool isSecondBoardEnabled() const { return _secondBoardEnabled; }
  // Etat reel du sondage I2C effectue par beginSafe(), expose tel quel par les
  // diagnostics (plus de "hardware probe requires device" alors que le firmware
  // connait deja la reponse).
  bool isPca0Detected() const { return _pca0Detected; }
  bool isPca1Detected() const { return _pca1Detected; }
  bool isSecondBoardRequired() const { return _secondBoardEnabled; }
  bool hardwareProbeDone() const { return _hardwareProbeDone; }
  ConfigApplyResult applyRuntimeConfig(const RuntimeConfig& oldConfig, const RuntimeConfig& newConfig);
  // Predicat PUR : la transition de configuration demande-t-elle une re-init
  // hardware (donc un reboot) ? Extrait de applyRuntimeConfig() pour que le
  // commit transactionnel puisse decider AVANT de toucher quoi que ce soit.
  static bool configChangeRequiresRestart(const RuntimeConfig& oldConfig, const RuntimeConfig& newConfig);

private:
  Adafruit_PWMServoDriver _pwm0;    // Carte 1 (0x40) — toujours active
  Adafruit_PWMServoDriver _pwm1;    // Carte 2 (0x41) — active si canaux >= 16
  bool _secondBoardEnabled;
  bool _pca0Detected;
  bool _pca1Detected;
  bool _hardwareProbeDone;
  HardwareInitStatus _hardwareInitStatus;
  EventQueue _eventQueue;
  CommandQueue _commands;
  FingerController _fingerCtrl;
  AirflowController _airflowCtrl;
  PressureController _pressureCtrl;
  FanController _fanCtrl;
  CalibrationAirSupply _calAirSupply;
  NoteSequencer _sequencer;

  unsigned long _lastActivityTime;
  bool _servosPowered;
  bool _actuatorSessionActive;   // calibration/range-finder holds power (see managePower)
  bool _initializingHardware;    // during beginSafe(): write safe PWM but never enable OE

  // Valeurs Control Change MIDI
  byte _ccVolume;
  byte _ccExpression;
  byte _ccModulation;
  byte _ccBreath;
  byte _ccBrightness;

  // Rate limiting
  uint16_t _ccCount;
  unsigned long _ccWindowStart;
  uint16_t _cc2Count;
  unsigned long _cc2WindowStart;
  // CC2 (Breath) : coalescence plutot que rejet. Quand la fenetre de debit est
  // saturee, la DERNIERE valeur recue est conservee ici et appliquee des que la
  // fenetre se rouvre. Une frequence elevee de CC2 ne peut donc plus laisser une
  // note soufflee alors que le controleur a deja demande zero.
  bool _cc2Pending;
  byte _cc2PendingValue;
  // --- Demandes deposees par une autre tache ---------------------------------
  // Ces deux drapeaux etaient `volatile`. `volatile` N'EST PAS une primitive de
  // synchronisation : il interdit au compilateur de mettre la variable en cache,
  // rien de plus - ni atomicite, ni barriere. La sequence "si le drapeau est
  // pose, l'effacer" tenait donc en deux acces distincts, et une demande deposee
  // par la tache AsyncTCP ENTRE les deux etait effacee sans avoir ete traitee :
  // un CC121 "Reset All Controllers" disparaissait en silence, ou les servos
  // restaient non alimentes.
  //
  // Ils sont maintenant poses et PRIS sous `_requestMux` (voir takePowerOnRequest
  // et takeResetControllersRequest), ce qui rend la lecture-puis-effacement
  // atomique, exactement comme CommandQueue le fait deja pour le panic.
  //
  // Verrou PROPRE, et non celui de CommandQueue : registerActuatorActivity() est
  // appelee depuis setPWM(), c'est-a-dire a CHAQUE ecriture de servo. Faire
  // passer ce chemin brulant par le verrou de la file le mettrait en concurrence
  // avec tous les push() de la tache AsyncTCP, pour deux etats qui n'ont rien a
  // voir. Sur ESP32, portENTER_CRITICAL desactive les interruptions : ces
  // sections doivent rester minuscules - ici, l'ecriture ou la lecture d'un seul
  // booleen, jamais un appel de controleur.
  //
  // Demande d'alimentation servo differee : registerActuatorActivity() peut etre
  // appelee hors de la tache loop() ; l'ecriture GPIO de l'OE est faite par
  // managePower() sur la tache proprietaire.
  bool _powerOnRequested;
  // CC121 Reset All Controllers poste depuis une autre tache : drapeau dedie pour
  // qu'il ne puisse jamais etre perdu par saturation de la file.
  bool _resetControllersRequested;
  mutable portMUX_TYPE _requestMux = portMUX_INITIALIZER_UNLOCKED;
  // Nombre de passes consecutives pendant lesquelles les Note Off en attente ont
  // cede le pas a l'anneau non draine (voir INSTRUMENT_MAX_NOTE_OFF_DEFERRAL_PASSES).
  // Lu et ecrit par la seule tache loop().
  uint8_t _noteOffDeferrals;
  // Compteur de paniques REELLEMENT executees (voir panicCount()).
  uint32_t _panicCount;

  void managePower();
  // Execute REELLEMENT une panique : compte puis eteint tout.
  //
  // Ce passage obligatoire est ce qui distingue une panique des autres appels a
  // allSoundOff(). allSoundOff() est aussi le "mettre en securite" ordinaire du
  // firmware : le lecteur MIDI l'appelle sur pause et sur fin de morceau, et le
  // serveur web avant un redemarrage controle (reset, reset usine, formatage).
  // Aucun de ces cas n'est une panique et aucun ne doit annuler une calibration
  // en cours ; compter dans allSoundOff() elle-meme les aurait tous comptes.
  void executePanic();
  // Applique une commande deja retiree de la file (tache loop() uniquement).
  void applyCommand(const ActuatorCommand& cmd);
  void processCommands();
  // Applique une valeur CC2 (souffle) sans repasser par le limiteur de debit.
  void applyBreathValue(byte ccValue);
  void serviceCc2Coalescing(unsigned long now);
  void powerOffServos();
  bool detectPca(uint8_t address) const;
  bool requiresSecondPca() const;

  // Air-source demand (0-100%) for the note velocity currently owned by the
  // sequencer, per air mode. Kept as helpers so update() can (re)apply the demand
  // on the sequencer's real note transitions.
  uint8_t computePumpDemand(byte velocity) const;
  uint8_t computeFanDemand(byte velocity) const;
  // Drive the direct pump / fan from the sequencer's real note transitions.
  void updateAirSourceFromSequencer();

  // Air source (pump/fan): track sequencer state transitions AND whether the held
  // note is actually sounding. CC2 (breath) can silence a held note without any
  // sequencer transition: the demand has to follow that too, otherwise the pump
  // keeps pushing against a valve the breath controller just closed.
  NoteState _prevSequencerState;
  bool _prevNoteSounding;

  // Observateur de chronometrie (PHASE 7). nullptr = aucun, et c'est le defaut.
  AcousticTiming* _timingObserver;
  // Observateur audio (PHASE 7). nullptr = aucun, et c'est le defaut.
  IAudioSource* _audioObserver;
};

#endif

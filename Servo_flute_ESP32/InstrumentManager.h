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
  bool postCommand(const ActuatorCommand& cmd);
  bool postCommand(uint8_t type, uint8_t a = 0, uint8_t b = 0, uint16_t c = 0);
  // Panic asynchrone : jamais perdu, prioritaire sur toute commande en attente.
  void requestPanic();
  const CommandQueue& commandQueue() const { return _commands; }
  uint16_t droppedCommandCount() const { return _commands.droppedCount(); }
  // Vrai si la commande touche physiquement un actionneur : refusee tant que le
  // hardware n'est pas pret (voir isHardwareReady()).
  static bool commandDrivesActuators(uint8_t type);
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
  // Demande d'alimentation servo differee : registerActuatorActivity() peut etre
  // appelee hors de la tache loop() ; l'ecriture GPIO de l'OE est faite par
  // managePower() sur la tache proprietaire.
  volatile bool _powerOnRequested;
  // CC121 Reset All Controllers poste depuis une autre tache : drapeau dedie pour
  // qu'il ne puisse jamais etre perdu par saturation de la file.
  volatile bool _resetControllersRequested;
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

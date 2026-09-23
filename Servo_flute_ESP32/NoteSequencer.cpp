#include "NoteSequencer.h"
#include "AcousticTiming.h"
#include "IAudioSource.h"
#include "ConfigStorage.h"

NoteSequencer::NoteSequencer(EventQueue& eventQueue, FingerController& fingerCtrl, AirflowController& airflowCtrl)
  : _eventQueue(eventQueue), _fingerCtrl(fingerCtrl), _airflowCtrl(airflowCtrl),
    _currentState(STATE_IDLE), _currentNote(0), _currentVelocity(0),
    _stateStartTime(0), _eventScheduledTime(0), _playbackStartTime(0),
    _noteSoundStartTime(0), _pendingStopAfterMinDuration(false),
    _timing(nullptr), _audio(nullptr) {
}

/*----------------------------------------------------------------------------
 * Notifications de chronometrie - PHASE 7
 *
 * Deux regles, et elles ne se negocient pas :
 *   1. test de nullite systematique : l'observateur est optionnel ;
 *   2. la valeur rendue est jetee (cast explicite en void). Ces hooks rendent
 *      un bool qui signale un ordre HORS SEQUENCE ; AcousticTiming le compte
 *      deja dans rejectedEvents(). Le lire ici pour en tirer une decision
 *      d'actionneur donnerait au moteur audio un pouvoir sur la mecanique, ce
 *      que le cahier des charges interdit.
 *
 * LE CHANGEMENT DE NOTE, signale a l'analyse aux deux MEMES bornes
 * -----------------------------------------------------------------
 * AudioAnalyzer::resetAcousticTracking() remet a zero la ligne de base de
 * brillance du detecteur de couac et l'historique de pitch. Elle n'avait, sur
 * le chemin de JEU, aucun appelant : seule une note VISEE la declenchait, et
 * seul AutoCalibrator en declare une. En lecture MIDI ordinaire le suivi
 * traversait donc les notes, et un legato montant d'une octave - musique
 * ordinaire - faisait publier ACOUSTIC_SQUEAK pendant six frames sur une note
 * propre (le couac est 3e dans l'ordre de priorite : il masque tout ce qui
 * suit) et `stability` a 0,00 AVEC `stabilityValid` vrai pendant sept frames.
 *
 * POURQUOI LES DEUX BORNES, et pas seulement le debut :
 *   - le DEBUT est celui qui repare le defaut : la remise a zero tombe a
 *     l'ordre de note, donc AVANT la premiere frame sonore de la note neuve,
 *     qui repart d'une ardoise vierge ;
 *   - la FIN est la seule qui couvre le cas ou aucune note ne suit - fin de
 *     phrase, panic, All Sound Off, transport perdu, prise de possession par
 *     le calibrateur. SqueakDetector se repare tout seul dans ce cas
 *     (forgetHeldNote() sur la premiere frame sans son), mais PAS l'historique
 *     de pitch : PitchDetector::detect() n'y empile que les mesures FIABLES et
 *     n'efface rien sur le silence, donc les cents de la note coupee y
 *     resteraient jusqu'a une hypothetique note suivante. Ce qui sonnerait
 *     entre-temps devant le moniteur serait mesure contre eux ;
 *   - le cout est borne et a SENS UNIQUE : resetAcousticTracking() ne peut que
 *     RETENIR un verdict (`stabilityValid` faux, `classified` faux), jamais en
 *     fabriquer un. Ce qu'elle coute, c'est la stabilite des frames d'extinction
 *     de la note qu'on relache - que personne ne consomme ; ce qu'elle evite,
 *     c'est un verdict FAUX ;
 *   - sur le remplacement monophonique les deux bornes tirent dans la meme
 *     passe de processDueEvents(), a quelques microsecondes : "les deux bornes"
 *     n'y coute exactement rien.
 *
 * Le hook rend VOID. Il n'y a donc meme pas de valeur a jeter : le sens unique
 * est garanti par le type, pas par une convention d'appel.
 *--------------------------------------------------------------------------*/

void NoteSequencer::notifyNoteCommanded() {
  if (_timing != nullptr) _timing->noteCommanded(millis());
  if (_audio != nullptr) _audio->resetAcousticTracking();
}

void NoteSequencer::notifyNoteReleased() {
  if (_timing != nullptr) (void)_timing->noteReleased(millis());
  if (_audio != nullptr) _audio->resetAcousticTracking();
}

void NoteSequencer::begin() {
  _currentState = STATE_IDLE;

  if (DEBUG) {
    Serial.println("DEBUG: NoteSequencer - Initialisation");
  }
}

void NoteSequencer::update() {
  processDueEvents();
  switch (_currentState) {
    case STATE_IDLE:
      // processDueEvents() ci-dessus a deja traite la file ; rien de plus a faire.
      break;
    case STATE_POSITIONING:
      handlePositioning();
      break;
    case STATE_PLAYING:
      handlePlaying();
      break;
    case STATE_STOPPING:
      handleStopping();
      break;
  }
}

NoteState NoteSequencer::getState() const {
  return _currentState;
}

bool NoteSequencer::isPlaying() const {
  return _currentState == STATE_PLAYING;
}

void NoteSequencer::handlePositioning() {
  unsigned long elapsed = millis() - _stateStartTime;

  if (elapsed >= cfg.servoToSolenoidDelayMs) {
    // Open the valve only if the note should actually sound. When CC2 (breath)
    // requests silence at the onset, setAirflowForNote() returns false and the
    // valve must stay closed instead of being force-opened.
    bool sound = _airflowCtrl.setAirflowForNote(_currentNote, _currentVelocity);
    if (sound) _airflowCtrl.openSolenoid();
    else _airflowCtrl.closeSolenoid();
    _noteSoundStartTime = millis();
    // NB: on ne remet PAS _pendingStopAfterMinDuration a false ici. Un Note Off
    // recu pendant le POSITIONING (note plus courte que la fenetre de
    // positionnement) l'a arme pour que la note sonne quand meme au moins
    // minNoteDurationMs. Il est deja remis a false a chaque nouveau Note On.
    transitionTo(STATE_PLAYING);

    if (DEBUG) {
      unsigned long actualTime = millis() - _playbackStartTime;
      unsigned long targetTime = _eventScheduledTime - _playbackStartTime;
      long timing_error = (long)actualTime - (long)targetTime;

      Serial.print("DEBUG: NoteSequencer - SON produit note ");
      Serial.print(_currentNote);
      Serial.print(" | Erreur: ");
      Serial.print(timing_error);
      Serial.println("ms");
    }
  }
}

void NoteSequencer::handlePlaying() {
  // Comparaison signee 32 bits explicite : sure au rollover de millis() et
  // identique sur l'hote des tests (ou `long` fait 64 bits) et sur l'ESP32.
  if (_pendingStopAfterMinDuration && (int32_t)(millis() - (_noteSoundStartTime + cfg.minNoteDurationMs)) >= 0) {
    _pendingStopAfterMinDuration = false;
    stopCurrentNote();
    return;   // la note est deja partie en STOPPING : pas de plafond a tester
  }

  /*--------------------------------------------------------------------------
   * PLAFOND DE DUREE DE NOTE
   *
   * Le defaut qu'il repare : il n'existait AUCUNE limite de duree de note.
   * Un Note Off qui n'arrive jamais laissait indefiniment la valve ouverte, la
   * bobine a son PWM de maintien et la pompe en regime. Le cas reproduit est un
   * MIDI DIN dont la source n'emet pas d'Active Sensing : SerialMidiHandler ne
   * peut alors rien detecter (sa perte de lien est conditionnee a la reception
   * d'un 0xFE), et cfg.timeUnpower ne coupe l'OE que lorsque le sequenceur est
   * DEJA au repos - donc jamais pendant une note tenue. Aucun autre garde-fou
   * ne couvrait ce cas : la BLE, la rtpMIDI et le Wi-Fi ont leurs propres
   * detections de deconnexion, le DIN muet n'en a pas.
   *
   * Le chronometre part de _noteSoundStartTime, l'instant ou la note SONNE
   * reellement (valve commandee) : c'est celui ou les actionneurs sont
   * reellement sollicites, et c'est deja la reference de cfg.minNoteDurationMs.
   * STATE_POSITIONING n'a pas besoin de plafond : il se termine tout seul apres
   * cfg.servoToSolenoidDelayMs, valeur bornee par le validateur. La duree
   * maximale de sollicitation est donc NOTE_HOLD_CEILING_MS + ce delai.
   *
   * POURQUOI stop() ET PAS stopCurrentNote(). stopCurrentNote() passe par
   * shouldCloseValveBetweenNotes(), qui peut decider de LAISSER LA VALVE
   * OUVERTE quand un Note On attend a moins de cfg.minNoteIntervalForValveCloseMs.
   * Or, quand ce plafond est atteint, plus rien ne prouve que le lien qui a
   * depose ces evenements est encore vivant : leur faire confiance rouvrirait
   * la valve dans la foulee et le plafond serait defait dans le tour suivant.
   * stop() est le chemin d'arret COMPLET que le projet possede deja (celui de
   * l'All Sound Off) et il ne se negocie pas : file videe - donc epoque
   * incrementee, aucun evenement anterieur ne peut encore etre joue -, valve
   * fermee sans condition, souffle et angle rendus au repos, note relachee
   * notifiee aux observateurs, retour a STATE_IDLE. Ce retour a STATE_IDLE est
   * ce qui rend enfin effectif cfg.timeUnpower (managePower() coupe alors l'OE,
   * donc les servos de doigts aussi) et ce que InstrumentManager lit comme une
   * fin de note pour ramener la pompe / le ventilateur a leur repos.
   *
   * Cout assume : un bourdon tenu VOLONTAIREMENT au-dela du plafond est coupe
   * et ne repart pas tout seul - il faut un nouveau Note On. C'est le choix
   * "materiel d'abord" : on ne distingue pas, de l'exterieur, une pedale tenue
   * d'un cable debranche.
   *------------------------------------------------------------------------*/
  if ((int32_t)(millis() - (_noteSoundStartTime + NOTE_HOLD_CEILING_MS)) >= 0) {
    if (DEBUG) {
      Serial.print("DEBUG: NoteSequencer - PLAFOND de duree atteint sur la note ");
      Serial.print(_currentNote);
      Serial.println(" -> extinction complete");
    }
    stop();
  }
}

void NoteSequencer::handleStopping() {
  transitionTo(STATE_IDLE);
}

void NoteSequencer::processDueEvents() {
  // L'epoque de la file est capturee AVANT la salve. Un clear() concurrent
  // (panic / All Sound Off / demarrage d'une session d'actionneurs) l'incremente :
  // on abandonne alors immediatement la salve pour qu'aucun evenement de l'ancien
  // contexte ne soit encore execute apres le panic.
  const uint32_t startEpoch = _eventQueue.epoch();
  // Reference de lecture capturee AVANT tout retrait : une fois la file videe,
  // getReferenceTime() repasse a 0 (elle ne sert qu'aux traces de timing).
  const unsigned long queueReference = _eventQueue.getReferenceTime();

  MidiEvent due;
  uint32_t popEpoch = startEpoch;
  // tryPopDueEvent() lit l'echeance ET retire le MEME evenement sous le MEME
  // verrou : plus aucune fenetre entre "regarder" et "consommer".
  while (_eventQueue.tryPopDueEvent(millis(), cfg.servoToSolenoidDelayMs, due, &popEpoch)) {
    if (popEpoch != startEpoch) {
      // La file a ete videe pendant la salve : l'evenement qu'on vient de retirer
      // appartient au nouveau contexte, mais la politique de panic prime. On le
      // laisse tomber et on sort ; le prochain update() repartira proprement.
      return;
    }

    if (_playbackStartTime == 0) {
      _playbackStartTime = (queueReference != 0) ? queueReference : due.timestamp;
    }

    if (due.type == EVENT_NOTE_ON) {
      // Monophonic policy: any due NOTE_ON has priority over min duration and replaces
      // the current note/positioning immediately so no stale NOTE_OFF can block the FIFO.
      _pendingStopAfterMinDuration = false;
      if (_currentState != STATE_IDLE) {
        stopCurrentNoteForReplacement();
      }
      startNoteSequence(due.midiNote, due.velocity, due.timestamp);
      continue;
    }

    if (due.type == EVENT_NOTE_OFF) {
      // Old NOTE_OFF events for notes already replaced are intentionally ignored.
      if ((_currentState == STATE_PLAYING || _currentState == STATE_POSITIONING) && due.midiNote == _currentNote) {
        if (_currentState == STATE_PLAYING) {
          unsigned long playedFor = millis() - _noteSoundStartTime;
          if (playedFor < cfg.minNoteDurationMs) {
            _pendingStopAfterMinDuration = true;
          } else {
            stopCurrentNote();
          }
        } else {
          // POSITIONING : la valve n'est pas encore ouverte. Si le Note Off est
          // reellement posterieur au Note On (vraie note courte, plus breve que
          // la fenetre de positionnement), on differe l'arret pour que la note
          // sonne quand meme minNoteDurationMs une fois lancee. Si le Note Off
          // est simultane au Note On (duree nulle/degenere), on annule direct.
          if (due.timestamp > _eventScheduledTime) {
            _pendingStopAfterMinDuration = true;
          } else {
            stopCurrentNote();
          }
        }
      }
      continue;
    }
  }
}

void NoteSequencer::startNoteSequence(byte note, byte velocity, unsigned long scheduledTime) {
  _currentNote = note;
  _currentVelocity = velocity;
  _eventScheduledTime = scheduledTime;

  _fingerCtrl.setFingerPatternForNote(note);
  // ORDRE DE NOTE ACCEPTE. C'est ICI, et pas a l'arrivee du message MIDI : une
  // note refusee (hardware en panne, calibration en cours, note hors plage) ou
  // perdue (file pleine, file videe par un panic entre-temps) n'atteint jamais
  // cette ligne et n'entre donc pas dans les statistiques de latence. L'instant
  // retenu est celui ou la chaine d'actionneurs part reellement : le motif de
  // doigte vient d'etre ecrit, l'air et la valve suivront apres la fenetre de
  // positionnement.
  notifyNoteCommanded();
  transitionTo(STATE_POSITIONING);

  if (DEBUG) {
    Serial.print("DEBUG: NoteSequencer - Debut sequence note: ");
    Serial.print(note);
    Serial.print(" (vel: ");
    Serial.print(velocity);
    Serial.println(")");
  }
}

bool NoteSequencer::shouldCloseValveBetweenNotes() {
  // Modes without physical valve: no need to close
  if (cfg.airMode == AIR_MODE_SERVO_ONLY || cfg.airMode == AIR_MODE_FAN_SERVO) {
    return true;  // Will call closeValve which is a no-op, but sets rest angle
  }

  // Copie par valeur : l'evenement suivant peut etre retire ou evince par une
  // autre tache juste apres cette lecture ; on ne garde donc jamais de pointeur.
  MidiEvent nextEvent;
  if (!_eventQueue.peekCopy(nextEvent) || nextEvent.type != EVENT_NOTE_ON) {
    return true;
  }

  unsigned long currentTime = millis();
  unsigned long nextNoteTime = nextEvent.timestamp;

  if (nextNoteTime > currentTime) {
    unsigned long interval = nextNoteTime - currentTime;
    if (interval < cfg.minNoteIntervalForValveCloseMs) {
      if (DEBUG) {
        Serial.print("DEBUG: NoteSequencer - Valve GARDEE ouverte (note suivante dans ");
        Serial.print(interval);
        Serial.println("ms)");
      }
      return false;
    }
  }

  return true;
}

void NoteSequencer::stopCurrentNoteForReplacement() {
  bool closeValve = shouldCloseValveBetweenNotes();

  if (closeValve) {
    _airflowCtrl.closeSolenoid();
    _airflowCtrl.setAirflowToRest();
  } else {
    _airflowCtrl.setAirflowVelocity(1);
  }

  // Monophonie : l'ancienne note est CLOSE avant que la nouvelle ne s'ouvre.
  // Sans cette notification, noteCommanded() de la note suivante fermerait bien
  // la precedente (TIMING_ABORTED) mais sans jamais horodater son ordre d'arret,
  // et les deux notes se chevaucheraient dans le releve. La notification vient
  // APRES l'action sur l'actionneur : la mise en securite ne depend de rien.
  notifyNoteReleased();
}

void NoteSequencer::stopCurrentNote() {
  bool closeValve = shouldCloseValveBetweenNotes();

  if (closeValve) {
    _airflowCtrl.closeSolenoid();
    _airflowCtrl.setAirflowToRest();
  } else {
    _airflowCtrl.setAirflowVelocity(1);
  }

  // ORDRE D'ARRET REEL. Pas celui du message Note Off : une note plus courte que
  // minNoteDurationMs voit son arret DIFFERE (_pendingStopAfterMinDuration), et
  // c'est ce passage-ci, une fois la duree minimale ecoulee, qui commande
  // vraiment l'extinction.
  notifyNoteReleased();

  transitionTo(STATE_STOPPING);
}

void NoteSequencer::transitionTo(NoteState newState) {
  _currentState = newState;
  _stateStartTime = millis();
}

void NoteSequencer::stop() {
  const bool hadNote = (_currentState != STATE_IDLE);
  _currentNote = 0;
  _currentVelocity = 0;
  _pendingStopAfterMinDuration = false;
  _eventQueue.clear();
  _airflowCtrl.closeSolenoid();
  _airflowCtrl.setAirflowToRest();
  // Panic / All Sound Off / transport perdu / prise de possession par le
  // calibrateur : la note en cours est coupee pour de bon. La notifier evite de
  // laisser la machine a etats de chronometrie armee jusqu'a son plafond de
  // 60 s. La mise en securite ci-dessus a deja eu lieu - on ne notifie qu'apres.
  // `hadNote` est lu sur l'etat du SEQUENCEUR, jamais sur l'observateur.
  if (hadNote) notifyNoteReleased();
  transitionTo(STATE_IDLE);

  if (DEBUG) {
    Serial.println("DEBUG: NoteSequencer - STOP force (All Sound Off)");
  }
}

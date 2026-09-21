#ifndef NOTE_SEQUENCER_H
#define NOTE_SEQUENCER_H

#include <Arduino.h>
#include "EventQueue.h"
#include "FingerController.h"
#include "AirflowController.h"
#include "settings.h"

// PHASE 7 : observateur de chronometrie acoustique. Declaration AVANCEE
// deliberee - le sequenceur n'a jamais besoin du type complet, il ne fait que
// detenir un pointeur eventuellement nul et l'appeler depuis le .cpp.
class AcousticTiming;

// PHASE 7 (suite) : observateur AUDIO. Meme declaration avancee, meme raison -
// le sequenceur n'a jamais besoin du type complet. Il ne lui signale qu'une
// chose, aux deux bornes de la note : la note a change.
class IAudioSource;

// Etats de la machine a etats pour une note
enum NoteState {
  STATE_IDLE,              // Aucune note en cours
  STATE_POSITIONING,       // Servos en deplacement + attente stabilisation
  STATE_PLAYING,           // Note active, son produit
  STATE_STOPPING           // Arret en cours
};

class NoteSequencer {
public:
  NoteSequencer(EventQueue& eventQueue, FingerController& fingerCtrl, AirflowController& airflowCtrl);

  void begin();
  void update();

  NoteState getState() const;
  bool isPlaying() const;

  // The note the sequencer currently owns (positioning or playing) and its
  // velocity. Used to drive the air source (pump / fan) from the sequencer's real
  // note transitions rather than from raw incoming MIDI events.
  byte getCurrentNote() const { return _currentNote; }
  byte getCurrentVelocity() const { return _currentVelocity; }

  // Arrete immediatement toute lecture (pour All Sound Off)
  void stop();

  // --- PHASE 7 : observateur de chronometrie (OPTIONNEL) ---------------------
  // Pose par InstrumentManager::setTimingObserver(), nullptr par defaut.
  // Le flux est a SENS UNIQUE : le sequenceur NOTIFIE l'observateur aux instants
  // ou un ordre agit REELLEMENT sur la chaine d'actionneurs, et ne lit jamais
  // rien de lui. Aucune decision de cette classe - pas une, pas dans un cas
  // degrade - ne depend de sa presence ni de ce que ses hooks rendent.
  void setTimingObserver(AcousticTiming* obs) { _timing = obs; }
  AcousticTiming* timingObserver() const { return _timing; }

  // --- Observateur AUDIO (OPTIONNEL) ----------------------------------------
  // Pose par InstrumentManager::setAudioObserver(), nullptr par defaut.
  // EXACTEMENT la meme discipline que ci-dessus, et pour la meme raison : le
  // sequenceur NOTIFIE le changement de note aux deux bornes, et ne lit jamais
  // rien de l'analyse. Le hook rend void : il n'y a meme pas de valeur qu'une
  // decision d'actionneur pourrait lire.
  //
  // Les deux observateurs sont INDEPENDANTS : poser l'un n'impose pas l'autre,
  // et l'absence de l'un ne change rien au second. Ils sont notifies depuis les
  // deux MEMES helpers prives, donc aux deux MEMES instants - ceux ou l'ordre
  // agit reellement sur la chaine d'actionneurs.
  void setAudioObserver(IAudioSource* obs) { _audio = obs; }
  IAudioSource* audioObserver() const { return _audio; }

private:
  EventQueue& _eventQueue;
  FingerController& _fingerCtrl;
  AirflowController& _airflowCtrl;

  NoteState _currentState;
  byte _currentNote;
  byte _currentVelocity;
  unsigned long _stateStartTime;
  unsigned long _eventScheduledTime;
  unsigned long _playbackStartTime;
  unsigned long _noteSoundStartTime;
  bool _pendingStopAfterMinDuration;

  // Observateur de chronometrie, nullptr tant que personne n'en pose un.
  AcousticTiming* _timing;
  // Observateur audio, nullptr tant que personne n'en pose un.
  IAudioSource* _audio;

  // Notifications SORTANTES. Elles ignorent volontairement la valeur rendue par
  // les hooks : un ordre hors sequence est deja compte par AcousticTiming
  // lui-meme (rejectedEvents()), et surtout la suite du traitement d'actionneur
  // ne doit en aucun cas en dependre.
  //
  // UN SEUL helper par borne, pour les DEUX observateurs : c'est ce qui
  // garantit que le changement de note est signale a l'analyse exactement aux
  // instants deja verifies pour la chronometrie, et qu'aucune evolution ne peut
  // les desapparier.
  void notifyNoteCommanded();
  void notifyNoteReleased();

  void processDueEvents();
  void transitionTo(NoteState newState);
  void handlePositioning();
  void handlePlaying();
  void handleStopping();
  void startNoteSequence(byte note, byte velocity, unsigned long scheduledTime);
  void stopCurrentNote();
  void stopCurrentNoteForReplacement();
  bool shouldCloseValveBetweenNotes();
};

#endif

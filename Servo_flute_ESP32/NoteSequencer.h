#ifndef NOTE_SEQUENCER_H
#define NOTE_SEQUENCER_H

#include <Arduino.h>
#include "EventQueue.h"
#include "FingerController.h"
#include "AirflowController.h"
#include "settings.h"

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

  // Arrete immediatement toute lecture (pour All Sound Off)
  void stop();

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

  void processNextEvent();
  void transitionTo(NoteState newState);
  void handleIdle();
  void handlePositioning();
  void handlePlaying();
  void handleStopping();
  void startNoteSequence(byte note, byte velocity, unsigned long scheduledTime);
  void stopCurrentNote();
  bool shouldCloseValveBetweenNotes();
};

#endif

#ifndef EVENT_QUEUE_H
#define EVENT_QUEUE_H

#include <Arduino.h>

// Types d'evenements MIDI
enum EventType {
  EVENT_NONE,
  EVENT_NOTE_ON,
  EVENT_NOTE_OFF
};

// Structure d'un evenement MIDI avec timestamp
struct MidiEvent {
  EventType type;
  byte midiNote;
  byte velocity;
  unsigned long timestamp;  // Timestamp relatif en ms depuis premier evenement

  MidiEvent() : type(EVENT_NONE), midiNote(0), velocity(0), timestamp(0) {}

  MidiEvent(EventType t, byte note, byte vel, unsigned long ts)
    : type(t), midiNote(note), velocity(vel), timestamp(ts) {}
};

// File FIFO circulaire pour evenements MIDI
class EventQueue {
public:
  EventQueue(int capacity);

  // Ajoute un evenement avec timestamp relatif au premier evenement
  bool enqueue(EventType type, byte note, byte velocity, unsigned long absoluteTime);

  // Recupere le prochain evenement sans le retirer
  MidiEvent* peek();

  // Retire le prochain evenement de la queue
  void dequeue();

  // Verifie si la queue est vide
  bool isEmpty() const;

  // Verifie si la queue est pleine
  bool isFull() const;

  // Retourne le nombre d'evenements en attente
  int getCount() const;

  // Vide completement la queue
  void clear();

  // Obtient le timestamp de reference (premier evenement)
  unsigned long getReferenceTime() const;

private:
  MidiEvent* _events;
  int _capacity;
  int _head;      // Index d'ecriture
  int _tail;      // Index de lecture
  int _count;     // Nombre d'elements
  unsigned long _referenceTime;  // Timestamp du premier evenement (millis absolu)
  bool _hasReference;
};

#endif

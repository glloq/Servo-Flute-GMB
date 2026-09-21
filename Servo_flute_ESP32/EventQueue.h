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
  unsigned long timestamp;  // Absolute execution time in millis()

  MidiEvent() : type(EVENT_NONE), midiNote(0), velocity(0), timestamp(0) {}

  MidiEvent(EventType t, byte note, byte vel, unsigned long ts)
    : type(t), midiNote(note), velocity(vel), timestamp(ts) {}
};

// File FIFO circulaire pour evenements MIDI.
//
// CONTRAT DE CONCURRENCE
// ----------------------
// Les producteurs (callbacks BLE-MIDI, rtpMIDI, MIDI DIN, WebSocket/AsyncTCP,
// lecteur SMF) et le consommateur (NoteSequencer, tache loop()) vivent dans des
// taches FreeRTOS differentes. Toute operation qui touche a plus d'un champ
// (_head/_tail/_count/_referenceTime) s'execute donc sous section critique.
//
// IMPORTANT : aucune methode ne rend un pointeur vers le stockage interne.
// L'ancien couple peek() -> lecture -> dequeue() n'etait PAS atomique : entre la
// lecture du pointeur et le dequeue(), une autre tache pouvait appeler clear()
// (panic / All Sound Off), inserer un Note Off force (qui evince la tete quand la
// file est pleine) ou deplacer _tail. Le sequenceur pouvait alors executer un
// evenement obsolete, en perdre un, ou en reordonner deux. Les seules primitives
// de consommation sont desormais tryPopDueEvent() (lecture + retrait du MEME
// evenement sous le MEME verrou) et peekCopy() (copie par valeur).
class EventQueue {
public:
  explicit EventQueue(int capacity);
  ~EventQueue();

  // Ajoute un evenement live avec heure absolue millis()
  bool enqueue(EventType type, byte note, byte velocity, unsigned long absoluteTime);
  bool enqueueLiveEvent(EventType type, byte note, byte velocity);
  bool enqueueScheduledEvent(EventType type, byte note, byte velocity, unsigned long executeAtMs);

  // Variantes "forcees" : si la file est pleine, evincent l'evenement le plus
  // ancien pour garantir l'insertion. A reserver aux Note Off : perdre un
  // Note Off laisserait une note (et donc valve/souffle) bloquee.
  bool enqueueLiveEventForced(EventType type, byte note, byte velocity);
  bool enqueueScheduledEventForced(EventType type, byte note, byte velocity, unsigned long executeAtMs);

  // Copie (par valeur) le prochain evenement sans le retirer. Retourne false si
  // la file est vide. La copie ne peut jamais pointer sur un emplacement recycle.
  bool peekCopy(MidiEvent& out) const;

  // Operation atomique de consommation : sous UN SEUL verrou, lit la tete,
  // calcule son echeance et, si elle est echue, la copie dans `out` ET la retire.
  // `noteOnLeadMs` est l'avance appliquee aux NOTE_ON (delai servos->valve).
  // Retourne false sans rien retirer si la file est vide ou si la tete n'est pas
  // encore due. `epochOut`, si fourni, recoit l'epoque de la file au moment du
  // retrait : le consommateur peut ainsi detecter un clear()/panic concurrent.
  bool tryPopDueEvent(unsigned long now, unsigned long noteOnLeadMs, MidiEvent& out,
                      uint32_t* epochOut = nullptr);

  // Retire le prochain evenement de la queue (sans le lire).
  void dequeue();

  // Verifie si la queue est vide
  bool isEmpty() const;

  // Verifie si la queue est pleine
  bool isFull() const;

  // Retourne le nombre d'evenements en attente
  int getCount() const;

  // Vide completement la queue et incremente l'epoque (invalide tout traitement
  // en cours cote consommateur).
  void clear();

  // Epoque courante : incrementee a chaque clear(). Un consommateur qui a
  // commence a traiter une salve d'evenements peut comparer l'epoque avant/apres
  // pour savoir qu'un panic a eu lieu entre-temps et abandonner la salve.
  uint32_t epoch() const;

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
  uint32_t _epoch;               // Incremente par clear()

  // Verrou de section critique : les callbacks HTTP/WS (tache AsyncTCP) et loop()
  // (tache principale) enfilent/defilent depuis DEUX taches FreeRTOS distinctes.
  // Sans protection, les mises a jour multi-champs de _head/_tail/_count peuvent
  // se corrompre (compteur qui deborde, index incoherent -> note bloquee).
  mutable portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;

  // Retrait de la tete. Doit etre appele AVEC le verrou deja pris.
  void popLocked();
};

#endif

#include "EventQueue.h"
#include <new>   // std::nothrow : une allocation ratee doit rendre nullptr, pas lever

EventQueue::EventQueue(int capacity)
  : _events(nullptr), _capacity(capacity > 0 ? capacity : 0), _head(0), _tail(0), _count(0),
    _referenceTime(0), _hasReference(false), _epoch(0) {
  if (_capacity > 0) {
    // std::nothrow : sur un tas ESP32 fragmente, un `new` nu leve une exception
    // qui, sans gestionnaire, redemarre la carte ; et le pointeur nul rendu par
    // une allocation ratee serait ensuite dereference a chaque enfilement.
    _events = new (std::nothrow) MidiEvent[_capacity];
  }
  // Allocation refusee (ou capacite nulle/negative demandee) : UN SEUL etat
  // degrade. L'ancien `capacity < 1 ? 1 : capacity` evitait le modulo par zero ;
  // les methodes sortent desormais AVANT tout calcul d'index quand le stockage
  // n'existe pas, ce qui couvre en plus l'allocation ratee.
  if (!_events) _capacity = 0;
}

EventQueue::~EventQueue() {
  delete[] _events;
}

bool EventQueue::enqueue(EventType type, byte note, byte velocity, unsigned long absoluteTime) {
  return enqueueScheduledEvent(type, note, velocity, absoluteTime);
}

bool EventQueue::enqueueLiveEvent(EventType type, byte note, byte velocity) {
  return enqueueScheduledEvent(type, note, velocity, millis());
}

bool EventQueue::enqueueScheduledEvent(EventType type, byte note, byte velocity, unsigned long executeAtMs) {
  // Section critique : cette methode est appelee depuis les callbacks web
  // (tache AsyncTCP) alors que loop() defile en parallele.
  portENTER_CRITICAL(&_mux);

  // Stockage absent (allocation refusee) : refus propre, jamais de deref.
  if (_events == nullptr || _count >= _capacity) {  // isFull() sans reprendre le verrou
    portEXIT_CRITICAL(&_mux);
    return false;
  }

  if (!_hasReference) {
    _referenceTime = executeAtMs;
    _hasReference = true;
  }

  _events[_head] = MidiEvent(type, note, velocity, executeAtMs);

  _head = (_head + 1) % _capacity;
  _count++;

  portEXIT_CRITICAL(&_mux);
  return true;
}

bool EventQueue::enqueueLiveEventForced(EventType type, byte note, byte velocity) {
  return enqueueScheduledEventForced(type, note, velocity, millis());
}

bool EventQueue::enqueueScheduledEventForced(EventType type, byte note, byte velocity, unsigned long executeAtMs) {
  portENTER_CRITICAL(&_mux);

  // Stockage absent : meme une insertion FORCEE doit repondre false. Il n'y a
  // aucun emplacement a evincer et il n'est pas question d'ecrire dans le vide ;
  // l'appelant (un Note Off) voit l'echec au lieu de croire la note relachee.
  if (_events == nullptr) {
    portEXIT_CRITICAL(&_mux);
    return false;
  }

  if (_count >= _capacity) {
    // File pleine : evincer le plus ancien pour faire de la place. La section
    // critique garde l'operation atomique vis-a-vis de loop().
    _tail = (_tail + 1) % _capacity;
    _count--;
  }

  if (!_hasReference) {
    _referenceTime = executeAtMs;
    _hasReference = true;
  }

  _events[_head] = MidiEvent(type, note, velocity, executeAtMs);
  _head = (_head + 1) % _capacity;
  _count++;

  portEXIT_CRITICAL(&_mux);
  return true;
}

void EventQueue::popLocked() {
  _tail = (_tail + 1) % _capacity;
  _count--;
  if (_count == 0) {
    _hasReference = false;
    _referenceTime = 0;
  }
}

bool EventQueue::peekCopy(MidiEvent& out) const {
  portENTER_CRITICAL(&_mux);
  if (_events == nullptr || _count == 0) {
    portEXIT_CRITICAL(&_mux);
    return false;
  }
  out = _events[_tail];
  portEXIT_CRITICAL(&_mux);
  return true;
}

bool EventQueue::tryPopDueEvent(unsigned long now, unsigned long noteOnLeadMs, MidiEvent& out,
                                uint32_t* epochOut) {
  portENTER_CRITICAL(&_mux);

  if (_events == nullptr || _count == 0) {
    portEXIT_CRITICAL(&_mux);
    return false;
  }

  const MidiEvent& head = _events[_tail];
  unsigned long dueTime = head.timestamp;
  if (head.type == EVENT_NOTE_ON) {
    // Les NOTE_ON sont avances du delai de positionnement des servos. Soustraction
    // saturante : une avance superieure a l'horodatage rend l'evenement du
    // immediatement (jamais un rollover vers un futur lointain).
    dueTime = (head.timestamp > noteOnLeadMs) ? head.timestamp - noteOnLeadMs : 0;
  }
  // Comparaison signee sur 32 bits EXPLICITES : reste correcte au rollover de
  // millis() (~49,7 jours). `long` ne convient pas : il fait 64 bits sur l'hote
  // des tests et la difference ne reboucle alors plus comme sur l'ESP32.
  if ((int32_t)(now - dueTime) < 0) {
    portEXIT_CRITICAL(&_mux);
    return false;
  }

  // Lecture ET retrait du MEME evenement, sous le MEME verrou.
  out = head;
  popLocked();
  if (epochOut) *epochOut = _epoch;

  portEXIT_CRITICAL(&_mux);
  return true;
}

void EventQueue::dequeue() {
  portENTER_CRITICAL(&_mux);
  // popLocked() calcule un index modulo _capacity : sans stockage, _capacity
  // vaut 0 et il ne doit jamais etre atteint.
  if (_events == nullptr || _count == 0) {  // isEmpty() sans reprendre le verrou
    portEXIT_CRITICAL(&_mux);
    return;
  }
  popLocked();
  portEXIT_CRITICAL(&_mux);
}

bool EventQueue::isEmpty() const {
  portENTER_CRITICAL(&_mux);
  bool empty = (_count == 0);
  portEXIT_CRITICAL(&_mux);
  return empty;
}

bool EventQueue::isFull() const {
  portENTER_CRITICAL(&_mux);
  bool full = (_count >= _capacity);
  portEXIT_CRITICAL(&_mux);
  return full;
}

int EventQueue::getCount() const {
  portENTER_CRITICAL(&_mux);
  int count = _count;
  portEXIT_CRITICAL(&_mux);
  return count;
}

void EventQueue::clear() {
  portENTER_CRITICAL(&_mux);
  _head = 0;
  _tail = 0;
  _count = 0;
  _hasReference = false;
  _referenceTime = 0;
  _epoch++;   // invalide toute salve de traitement en cours cote consommateur
  portEXIT_CRITICAL(&_mux);
}

uint32_t EventQueue::epoch() const {
  portENTER_CRITICAL(&_mux);
  uint32_t e = _epoch;
  portEXIT_CRITICAL(&_mux);
  return e;
}

unsigned long EventQueue::getReferenceTime() const {
  portENTER_CRITICAL(&_mux);
  unsigned long ref = _referenceTime;
  portEXIT_CRITICAL(&_mux);
  return ref;
}

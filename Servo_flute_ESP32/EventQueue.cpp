#include "EventQueue.h"

EventQueue::EventQueue(int capacity)
  : _capacity(capacity), _head(0), _tail(0), _count(0),
    _referenceTime(0), _hasReference(false) {
  _events = new MidiEvent[capacity];
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

  if (_count >= _capacity) {  // isFull() sans reprendre le verrou
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

MidiEvent* EventQueue::peek() {
  if (isEmpty()) {
    return nullptr;
  }
  return &_events[_tail];
}

void EventQueue::dequeue() {
  portENTER_CRITICAL(&_mux);
  if (_count == 0) {  // isEmpty() sans reprendre le verrou
    portEXIT_CRITICAL(&_mux);
    return;
  }

  _tail = (_tail + 1) % _capacity;
  _count--;

  if (_count == 0) {
    _hasReference = false;
    _referenceTime = 0;
  }
  portEXIT_CRITICAL(&_mux);
}

bool EventQueue::isEmpty() const {
  return _count == 0;
}

bool EventQueue::isFull() const {
  return _count >= _capacity;
}

int EventQueue::getCount() const {
  return _count;
}

void EventQueue::clear() {
  portENTER_CRITICAL(&_mux);
  _head = 0;
  _tail = 0;
  _count = 0;
  _hasReference = false;
  _referenceTime = 0;
  portEXIT_CRITICAL(&_mux);
}

unsigned long EventQueue::getReferenceTime() const {
  return _referenceTime;
}

#include "EventQueue.h"

EventQueue::EventQueue(int capacity)
  : _capacity(capacity), _head(0), _tail(0), _count(0),
    _referenceTime(0), _hasReference(false) {
  _events = new MidiEvent[capacity];
}

bool EventQueue::enqueue(EventType type, byte note, byte velocity, unsigned long absoluteTime) {
  if (isFull()) {
    return false;
  }

  if (!_hasReference) {
    _referenceTime = absoluteTime;
    _hasReference = true;
  }

  unsigned long relativeTime = absoluteTime - _referenceTime;

  _events[_head] = MidiEvent(type, note, velocity, relativeTime);

  _head = (_head + 1) % _capacity;
  _count++;

  return true;
}

MidiEvent* EventQueue::peek() {
  if (isEmpty()) {
    return nullptr;
  }
  return &_events[_tail];
}

void EventQueue::dequeue() {
  if (isEmpty()) {
    return;
  }

  _tail = (_tail + 1) % _capacity;
  _count--;

  if (_count == 0) {
    _hasReference = false;
    _referenceTime = 0;
  }
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
  _head = 0;
  _tail = 0;
  _count = 0;
  _hasReference = false;
  _referenceTime = 0;
}

unsigned long EventQueue::getReferenceTime() const {
  return _referenceTime;
}

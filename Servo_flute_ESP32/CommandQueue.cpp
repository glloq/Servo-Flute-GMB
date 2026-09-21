#include "CommandQueue.h"

CommandQueue::CommandQueue(uint8_t capacity)
  : _capacity(capacity < 1 ? 1 : capacity), _head(0), _tail(0), _count(0),
    _dropped(0), _panic(false) {
  _items = new ActuatorCommand[_capacity];
}

CommandQueue::~CommandQueue() {
  delete[] _items;
}

bool CommandQueue::push(const ActuatorCommand& cmd) {
  portENTER_CRITICAL(&_mux);
  if (_count >= _capacity) {
    if (_dropped < 0xFFFF) _dropped++;
    portEXIT_CRITICAL(&_mux);
    return false;
  }
  _items[_head] = cmd;
  _head = (uint8_t)((_head + 1) % _capacity);
  _count++;
  portEXIT_CRITICAL(&_mux);
  return true;
}

bool CommandQueue::pop(ActuatorCommand& out) {
  portENTER_CRITICAL(&_mux);
  if (_count == 0) {
    portEXIT_CRITICAL(&_mux);
    return false;
  }
  out = _items[_tail];
  _tail = (uint8_t)((_tail + 1) % _capacity);
  _count--;
  portEXIT_CRITICAL(&_mux);
  return true;
}

void CommandQueue::requestPanic() {
  portENTER_CRITICAL(&_mux);
  _panic = true;
  // Tout ce qui attendait avant le panic est abandonne : un test de pompe ou un
  // angle de servo mis en file avant l'arret d'urgence ne doit jamais s'appliquer
  // APRES lui.
  _head = 0;
  _tail = 0;
  _count = 0;
  portEXIT_CRITICAL(&_mux);
}

bool CommandQueue::takePanicRequest() {
  portENTER_CRITICAL(&_mux);
  bool p = _panic;
  _panic = false;
  portEXIT_CRITICAL(&_mux);
  return p;
}

bool CommandQueue::panicPending() const {
  portENTER_CRITICAL(&_mux);
  bool p = _panic;
  portEXIT_CRITICAL(&_mux);
  return p;
}

void CommandQueue::clear() {
  portENTER_CRITICAL(&_mux);
  _head = 0;
  _tail = 0;
  _count = 0;
  portEXIT_CRITICAL(&_mux);
}

uint8_t CommandQueue::count() const {
  portENTER_CRITICAL(&_mux);
  uint8_t c = _count;
  portEXIT_CRITICAL(&_mux);
  return c;
}

uint16_t CommandQueue::droppedCount() const {
  portENTER_CRITICAL(&_mux);
  uint16_t d = _dropped;
  portEXIT_CRITICAL(&_mux);
  return d;
}

void CommandQueue::resetDroppedCount() {
  portENTER_CRITICAL(&_mux);
  _dropped = 0;
  portEXIT_CRITICAL(&_mux);
}

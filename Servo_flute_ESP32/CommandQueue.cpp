#include "CommandQueue.h"
#include <new>   // std::nothrow : une allocation ratee doit rendre nullptr, pas lever

CommandQueue::CommandQueue(uint8_t capacity)
  : _items(nullptr), _capacity(capacity), _head(0), _tail(0), _count(0),
    _dropped(0), _panic(false) {
  if (_capacity > 0) {
    // std::nothrow : sur un tas ESP32 fragmente, un `new` nu leve une exception
    // qui, sans gestionnaire, redemarre la carte ; et le pointeur nul rendu par
    // une allocation ratee serait ensuite dereference a chaque push.
    _items = new (std::nothrow) ActuatorCommand[_capacity];
  }
  // Allocation refusee (ou capacite nulle demandee) : UN SEUL etat degrade, ce
  // qui rend les deux cas identiques et testables.
  //
  // PROTECTION REMPLACEE, pas retiree : l'ancien `capacity < 1 ? 1 : capacity`
  // servait a eviter un modulo par zero dans push()/pop(). Ces deux methodes
  // sortent desormais AVANT tout calcul d'index quand l'anneau n'existe pas, ce
  // qui couvre aussi l'allocation ratee - que la clause d'origine ne couvrait
  // pas du tout.
  if (!_items) _capacity = 0;
  for (uint8_t i = 0; i < 4; i++) _pendingNoteOff[i] = 0;
}

CommandQueue::~CommandQueue() {
  delete[] _items;
}

bool CommandQueue::push(const ActuatorCommand& cmd) {
  portENTER_CRITICAL(&_mux);
  // Anneau absent (allocation refusee) : on refuse et on COMPTE, exactement
  // comme une file pleine. La panne devient ainsi visible dans les diagnostics
  // au lieu d'etre muette - et aucun pointeur nul n'est deref.
  if (_items == nullptr || _count >= _capacity) {
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
  if (_items == nullptr || _count == 0) {
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
  // Un panic coupe deja tout : les relachements en attente n'ont plus d'objet.
  for (uint8_t i = 0; i < 4; i++) _pendingNoteOff[i] = 0;
  portEXIT_CRITICAL(&_mux);
}

void CommandQueue::requestNoteOff(uint8_t note) {
  if (note > 127) return;
  portENTER_CRITICAL(&_mux);
  _pendingNoteOff[note >> 5] |= (uint32_t)1UL << (note & 31);
  portEXIT_CRITICAL(&_mux);
}

bool CommandQueue::takePendingNoteOff(uint8_t& note) {
  bool found = false;
  portENTER_CRITICAL(&_mux);
  for (uint8_t w = 0; w < 4 && !found; w++) {
    if (_pendingNoteOff[w] == 0) continue;
    for (uint8_t b = 0; b < 32; b++) {
      if (_pendingNoteOff[w] & ((uint32_t)1UL << b)) {
        _pendingNoteOff[w] &= ~((uint32_t)1UL << b);
        note = (uint8_t)((w << 5) | b);
        found = true;
        break;
      }
    }
  }
  portEXIT_CRITICAL(&_mux);
  return found;
}

bool CommandQueue::hasPendingNoteOff() const {
  portENTER_CRITICAL(&_mux);
  bool any = (_pendingNoteOff[0] | _pendingNoteOff[1] | _pendingNoteOff[2] | _pendingNoteOff[3]) != 0;
  portEXIT_CRITICAL(&_mux);
  return any;
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
  // clear() n'est appele qu'au demarrage et par allSoundOff(), qui eteint deja
  // tout : un Note Off encore en attente n'a plus d'objet et serait applique sur
  // une note qui ne joue plus.
  for (uint8_t i = 0; i < 4; i++) _pendingNoteOff[i] = 0;
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

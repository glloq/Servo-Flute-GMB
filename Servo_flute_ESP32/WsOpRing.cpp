#include "WsOpRing.h"

WsOpRing::WsOpRing(uint8_t capacity, uint8_t maxPerPass)
  : _capacity(capacity), _maxPerPass(maxPerPass),
    _head(0), _tail(0), _count(0), _ranThisPass(0) {
  // Une capacite nulle rendrait le modulo indefini. Une borne nulle ferait une
  // file qui ne s'ecoule jamais - pire que pas de borne du tout. Les deux sont
  // ramenees a 1 : la file reste utilisable, degradee mais jamais bloquee.
  if (_capacity == 0) _capacity = 1;
  if (_maxPerPass == 0) _maxPerPass = 1;
}

bool WsOpRing::push(uint8_t& slot) {
  if (_count >= _capacity) return false;
  slot = _head;
  _head = (uint8_t)((_head + 1) % _capacity);
  _count++;
  return true;
}

bool WsOpRing::popForPass(uint8_t& slot) {
  if (_count == 0) return false;
  // LA BORNE. L'operation n'est PAS consommee : elle reste en tete, donc
  // l'ordre FIFO est conserve et rien n'est perdu.
  if (_ranThisPass >= _maxPerPass) return false;
  slot = _tail;
  _tail = (uint8_t)((_tail + 1) % _capacity);
  _count--;
  _ranThisPass++;
  return true;
}

void WsOpRing::beginPass() {
  _ranThisPass = 0;
}

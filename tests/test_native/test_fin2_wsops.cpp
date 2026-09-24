// Finalisation 2 - LOT 3 : le travail WebSocket fait par tour de loop() doit
// etre BORNE.
//
// LE DEFAUT, tel qu'il existe dans le depot
// -----------------------------------------
//     void WebConfigurator::serviceWsOps() {
//       while (true) { ... executeWebOp(op); ... }
//     }
//
// Six places, mais pas six operations equivalentes : `WEBOP_MIC_RESET` passe
// par `resetMicrophone()`, qui comporte un `delay(100)` et jusqu'a ~500 ms
// d'attente I2S ; le chargement d'un fichier MIDI, un commit de configuration
// et les acces LittleFS sont du meme ordre. Une passe pouvait donc retenir
// `loop()` plusieurs secondes - et repousser d'autant
// `InstrumentManager::update()`, qui est l'endroit ou un ordre d'ARRET ou un
// PANIC atteint reellement les actionneurs.
//
// CE QUI EST TESTE ICI, ET CE QUI NE PEUT PAS L'ETRE
// --------------------------------------------------
// La comptabilite d'indices et la borne vivent dans `WsOpRing`, pur : elles
// s'executent reellement ici. La charge utile (`WebOp`, qui porte des `String`
// Arduino) reste dans `WebConfigurator`, qu'aucun build hote ne compile ; le
// fait que `serviceWsOps()` passe bien par ce module - et non plus par un
// `while (true)` - est verrouille par une garde de source.
//
// Niveau de validation atteint : EXECUTE SUR HOTE. Rien n'a tourne sur ESP32.
#include <cassert>
#include <vector>
#include "WsOpRing.h"

namespace {

// Simule une passe de loop() : rend les emplacements reellement executes.
std::vector<uint8_t> runOnePass(WsOpRing& ring) {
  std::vector<uint8_t> done;
  ring.beginPass();
  uint8_t slot;
  while (ring.popForPass(slot)) done.push_back(slot);
  return done;
}

void six_operations_fit_and_a_seventh_is_refused() {
  WsOpRing ring(6, WS_OP_MAX_PER_PASS);
  uint8_t slot;
  for (int i = 0; i < 6; i++) {
    assert(ring.push(slot));
    assert(slot == (uint8_t)i);
  }
  assert(ring.count() == 6);
  assert(ring.full());
  // Pleine : le refus est celui d'avant, l'appelant rend false a son client.
  assert(!ring.push(slot));
  assert(ring.count() == 6);
}

// LE TEST CENTRAL : une passe n'en execute qu'un nombre borne, les autres
// restent intactes.
void one_pass_executes_a_bounded_number_and_leaves_the_rest() {
  WsOpRing ring(6, WS_OP_MAX_PER_PASS);
  uint8_t slot;
  for (int i = 0; i < 6; i++) assert(ring.push(slot));

  std::vector<uint8_t> first = runOnePass(ring);
  assert(first.size() == WS_OP_MAX_PER_PASS);
  // Les autres sont TOUJOURS LA. Rien n'est perdu, rien n'est execute en trop.
  assert(ring.count() == (uint8_t)(6 - WS_OP_MAX_PER_PASS));
  // ASSERTION QUI NE RE-ENCODE PAS LA CONSTANTE. Sans elle, ce test passerait
  // encore si quelqu'un remontait la borne a 6 - c'est-a-dire s'il supprimait
  // le bornage. "Borner" veut dire qu'il RESTE du travail apres une passe.
  assert(first.size() < 6);
  assert(ring.count() > 0);
}

// Rien n'est perdu et l'ordre d'emission est respecte de bout en bout.
void every_operation_runs_exactly_once_and_in_fifo_order() {
  WsOpRing ring(6, WS_OP_MAX_PER_PASS);
  uint8_t slot;
  for (int i = 0; i < 6; i++) assert(ring.push(slot));

  std::vector<uint8_t> order;
  int passes = 0;
  while (ring.count() > 0) {
    std::vector<uint8_t> done = runOnePass(ring);
    // Une passe ne peut jamais depasser la borne.
    assert(done.size() <= WS_OP_MAX_PER_PASS);
    // Une passe sur une file non vide fait forcement PROGRESSER : sans cela la
    // file ne s'ecoulerait jamais, ce qui serait pire que pas de borne.
    assert(done.size() >= 1);
    for (uint8_t s : done) order.push_back(s);
    passes++;
    assert(passes <= 20);   // garde-fou anti-boucle infinie du test lui-meme
  }
  // Six operations, six executions, dans l'ordre de depot.
  assert(order.size() == 6);
  for (int i = 0; i < 6; i++) assert(order[i] == (uint8_t)i);
  // Et il a bien fallu PLUSIEURS passes : c'est ce qui distingue une file
  // bornee d'une file drainee d'un coup. Ecrit sans la constante, pour la meme
  // raison que ci-dessus.
  assert(passes >= 2);
  assert(passes == (int)(6 / WS_OP_MAX_PER_PASS));
}

// La borne est par PASSE, pas globale : elle se remet a zero a chaque tour.
void the_bound_resets_on_each_pass() {
  WsOpRing ring(6, WS_OP_MAX_PER_PASS);
  uint8_t slot;
  for (int i = 0; i < 3; i++) assert(ring.push(slot));

  assert(runOnePass(ring).size() == WS_OP_MAX_PER_PASS);
  assert(runOnePass(ring).size() == WS_OP_MAX_PER_PASS);
  // Sans beginPass(), la seconde passe n'aurait rien execute du tout.
}

// La file continue d'accepter des depots PENDANT qu'elle s'ecoule, et l'ordre
// reste celui de l'emission - c'est le cas reel : AsyncTCP depose au fil de
// l'eau pendant que loop() consomme.
void pushes_during_the_drain_keep_their_place_in_line() {
  WsOpRing ring(6, WS_OP_MAX_PER_PASS);
  uint8_t slot;
  for (int i = 0; i < 3; i++) assert(ring.push(slot));   // places 0,1,2

  std::vector<uint8_t> order;
  for (uint8_t s : runOnePass(ring)) order.push_back(s); // consomme 0

  assert(ring.push(slot));                                // reutilise la place 3
  assert(slot == 3);
  for (uint8_t s : runOnePass(ring)) order.push_back(s);  // consomme 1
  for (uint8_t s : runOnePass(ring)) order.push_back(s);  // consomme 2
  for (uint8_t s : runOnePass(ring)) order.push_back(s);  // consomme 3

  assert(ring.count() == 0);
  assert(order.size() == 4);
  assert(order[0] == 0 && order[1] == 1 && order[2] == 2 && order[3] == 3);
}

// L'anneau boucle : apres un tour complet, les places sont reutilisees sans
// que l'ordre ne se melange.
void the_ring_wraps_without_reordering() {
  WsOpRing ring(3, WS_OP_MAX_PER_PASS);
  uint8_t slot;
  std::vector<uint8_t> order;
  for (int round = 0; round < 4; round++) {
    assert(ring.push(slot));
    assert(slot == (uint8_t)(round % 3));
    for (uint8_t s : runOnePass(ring)) order.push_back(s);
  }
  assert(order.size() == 4);
  assert(order[0] == 0 && order[1] == 1 && order[2] == 2 && order[3] == 0);
}

// Une file vide ne fait rien et ne compte pas de passe utilisee.
void an_empty_ring_does_nothing() {
  WsOpRing ring(6, WS_OP_MAX_PER_PASS);
  assert(runOnePass(ring).empty());
  assert(ring.count() == 0);
}

// Degradations impossibles a exprimer par construction : capacite ou borne
// nulles sont ramenees a 1 plutot que de produire un modulo par zero ou une
// file qui ne s'ecoule jamais.
void degenerate_parameters_stay_usable() {
  WsOpRing zeroCap(0, WS_OP_MAX_PER_PASS);
  assert(zeroCap.capacity() == 1);
  uint8_t slot;
  assert(zeroCap.push(slot));
  assert(!zeroCap.push(slot));
  assert(runOnePass(zeroCap).size() == 1);

  WsOpRing zeroBound(6, 0);
  assert(zeroBound.maxPerPass() == 1);
  assert(zeroBound.push(slot));
  assert(runOnePass(zeroBound).size() == 1);   // s'ecoule, ne se bloque pas
}

}  // namespace

void fin2_wsops_run_all_tests() {
  six_operations_fit_and_a_seventh_is_refused();
  one_pass_executes_a_bounded_number_and_leaves_the_rest();
  every_operation_runs_exactly_once_and_in_fifo_order();
  the_bound_resets_on_each_pass();
  pushes_during_the_drain_keep_their_place_in_line();
  the_ring_wraps_without_reordering();
  an_empty_ring_does_nothing();
  degenerate_parameters_stay_usable();
}

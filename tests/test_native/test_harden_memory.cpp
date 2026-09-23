// Durcissement memoire : les protections qui existaient sans pouvoir se
// declencher.
//
// Plusieurs allocations du firmware etaient suivies d'une gestion d'echec
// soignee - un test de nullite, un mode degrade, un code d'erreur HTTP - mais
// ecrites avec un `new` ORDINAIRE. Sur ESP32, exceptions desactivees, un tel
// `new` qui echoue ne rend pas nullptr : il abandonne et la carte redemarre.
// Aucune de ces protections ne pouvait donc s'executer. Elles se relisaient
// bien, elles ne protegeaient rien - exactement la meme illusion qu'un test
// defini mais jamais appele.
//
// Ces tests verrouillent l'autre moitie du correctif : que le mode degrade,
// une fois REELLEMENT atteignable, soit sur. Ce qui se verifie sur hote est le
// comportement du lecteur MIDI prive de son tableau d'evenements. Le fait que
// `new (std::nothrow)` reste en place, lui, est verrouille cote source par
// tests/test_static_audit.py (test_p2_no_bare_new_in_firmware_sources) : le
// harnais hote ne peut pas epuiser le tas d'un ESP32.
//
// NIVEAU DE VALIDATION : execute sur hote. Rien n'a tourne sur un ESP32.
#include <cassert>
#include <cstdio>

#include "MidiFilePlayer.h"

void harden_memory_run_all_tests();

// Un lecteur dont begin() n'a jamais reussi a allouer `_events` se trouve
// exactement dans l'etat d'un lecteur jamais initialise : c'est ce que le
// correctif rend atteignable, et c'est donc ce qu'on eprouve ici.
static void midi_player_without_events_is_inert_not_fatal() {
  MidiFilePlayer p;   // begin() JAMAIS appelee : _events == nullptr

  // Le point d'entree qui portait deja la protection.
  assert(p.loadFile("/midi/whatever.mid") == false);
  assert(p.isFileLoaded() == false);
  assert(p.getEventCount() == 0);

  // Et tous les autres, qui doivent rester inoffensifs plutot que de
  // dereferencer le tableau absent.
  p.play();
  assert(p.getState() == PLAYER_STOPPED);
  p.pause();
  assert(p.getState() == PLAYER_STOPPED);
  p.update();
  p.stop();
  assert(p.getState() == PLAYER_STOPPED);
  assert(p.isFileLoaded() == false);

  // Rejouer la sequence complete : un etat degrade doit etre STABLE, pas
  // seulement survivre au premier appel.
  for (int i = 0; i < 3; i++) {
    assert(p.loadFile("/midi/whatever.mid") == false);
    p.play(); p.update(); p.pause(); p.update(); p.stop();
    assert(p.getState() == PLAYER_STOPPED);
    assert(p.isFileLoaded() == false);
  }
}

// begin() rend desormais le succes de l'allocation. Sans cette valeur, un tas
// insuffisant se manifestait uniquement par des fichiers MIDI qui ne se
// chargeaient plus, sans que personne puisse dire pourquoi.
static void midi_player_begin_reports_whether_it_could_allocate() {
  MidiFilePlayer p;
  assert(p.begin(nullptr) == true);   // sur hote l'allocation aboutit toujours
  assert(p.isFileLoaded() == false);  // allouer n'est pas charger
  assert(p.getEventCount() == 0);

  // Un second begin() ne doit pas laisser le lecteur dans un etat incoherent.
  assert(p.begin(nullptr) == true);
  assert(p.isFileLoaded() == false);
}

void harden_memory_run_all_tests() {
  midi_player_without_events_is_inert_not_fatal();
  midi_player_begin_reports_whether_it_could_allocate();
}

#ifdef STANDALONE_TEST_MAIN
int main() { harden_memory_run_all_tests(); printf("harden memory tests passed\n"); return 0; }
#endif

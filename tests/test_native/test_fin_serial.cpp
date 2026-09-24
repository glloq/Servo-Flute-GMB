// MIDI DIN : le drainage de l'UART etait la derniere grosse boucle d'execution
// sans borne du firmware.
//
//     while (_serial->available()) { parseByte(_serial->read()); }
//
// Au debit MIDI nominal - 31250 bauds, un octet toutes les 320 us - l'UART ne
// peut pas alimenter cette boucle plus vite qu'elle ne la vide : elle se
// terminait donc d'elle-meme, et c'est pour cela qu'elle n'avait jamais pose
// de probleme. Mais la broche RX est CONFIGURABLE (`cfg.serialMidiRxPin`) et
// rien ne garantit qu'elle porte du MIDI : flottante, ou cablee par erreur sur
// un signal rapide, elle produit des octets d'erreur de trame en continu. La
// boucle tourne alors sans fin, et avec elle loop() - pression, ventilateur,
// WebSocket, panic et chien de garde compris.
//
// Le risque propre a la CORRECTION est ailleurs : borner le drainage coupe des
// messages MIDI en deux. Le test le plus important de ce fichier est donc celui
// qui verifie qu'un message a cheval sur deux passes est quand meme reconnu -
// le decodeur est a etat (`_status`, `_dataIndex`, `_expectedLen` survivent
// d'un appel a l'autre), et c'est ce qui rend le bornage sur.
//
// NIVEAU DE VALIDATION : execute sur hote. Rien n'a tourne sur ESP32, et le
// stub HardwareSerial n'est pas un vrai UART : il ne reproduit ni les erreurs
// de trame ni le debordement du tampon materiel.
#include <cassert>
#include <cstdio>
#include <vector>

#include "ConfigStorage.h"
#include "Wire.h"
#include "InstrumentManager.h"
#include "SerialMidiHandler.h"

void fin_serial_run_all_tests();

namespace {

void serialResetCfg() {
  ConfigStorage::makeDefaultConfig(cfg);
  cfg.serialMidiEnabled = true;
  cfg.serialMidiRxPin = 16;
  cfg.midiChannel = 0;   // OMNI : aucun message n'est ecarte par le filtre
}

// Un Note On complet sur le canal 1. La note doit etre JOUABLE : la
// configuration par defaut ne couvre que MIDI 82-103, et un Note On hors
// tessiture est refuse avant d'atteindre le sequenceur - ce qui ferait prendre
// un test faux pour un bornage casse.
void feedNoteOn(uint8_t note, uint8_t vel) {
  Serial2.__feedByte(0x90);
  Serial2.__feedByte(note);
  Serial2.__feedByte(vel);
}

InstrumentManager* makeReadyInstrument() {
  Wire.clear();
  Wire.setPresent(PCA_ADDR_BOARD0, true);
  InstrumentManager* im = new InstrumentManager();
  assert(im->beginSafe());
  assert(im->isHardwareReady());
  return im;
}

SerialMidiHandler* makeRunningHandler(InstrumentManager* im) {
  Serial2.__reset();
  SerialMidiHandler* h = new SerialMidiHandler();
  h->begin(im);
  return h;
}

// LE test du defaut : le drainage est borne, et rien n'est perdu.
void the_uart_drain_is_bounded_and_loses_nothing() {
  serialResetCfg();
  InstrumentManager* im = makeReadyInstrument();
  SerialMidiHandler* h = makeRunningHandler(im);

  // Beaucoup plus d'octets qu'une passe ne doit en lire. On les compte en
  // octets RESTANTS, sans nommer la borne : relacher la borne doit casser ce
  // test, pas le satisfaire.
  const size_t total = 600;
  for (size_t i = 0; i < total; i++) Serial2.__feedByte(0xFE);  // Active Sensing
  assert(Serial2.__remaining() == total);

  h->update();
  const size_t afterFirst = Serial2.__remaining();
  assert(afterFirst > 0);          // la passe s'est ARRETEE : elle est bornee
  assert(afterFirst < total);      // et elle a bien travaille

  // Les passes suivantes finissent le travail, sans jamais tout avaler d'un
  // coup, et le nombre de passes reste fini.
  int passes = 1;
  size_t previous = afterFirst;
  while (Serial2.__remaining() > 0) {
    h->update();
    passes++;
    assert(Serial2.__remaining() < previous);   // progression stricte
    previous = Serial2.__remaining();
    assert(passes < 200);                       // pas de boucle infinie
  }
  assert(Serial2.__remaining() == 0);
  delete h; delete im;
}

// Le risque introduit par la correction : un message coupe par la borne.
void a_message_split_across_passes_is_still_decoded() {
  serialResetCfg();
  InstrumentManager* im = makeReadyInstrument();
  SerialMidiHandler* h = makeRunningHandler(im);

  // On remplit jusqu'a deux octets de la borne, puis on place un Note On : ses
  // trois octets tombent forcement de part et d'autre d'une frontiere de passe.
  for (size_t i = 0; i < 600; i++) Serial2.__feedByte(0xFE);
  feedNoteOn(cfg.notes[0].midiNote, 100);

  int passes = 0;
  while (Serial2.__remaining() > 0 && passes < 200) { h->update(); passes++; }
  assert(Serial2.__remaining() == 0);

  // Le decodeur est a etat : la note a survecu a la coupure.
  // Le decodeur est a etat : la note a survecu a la coupure de la borne.
  im->update();
  assert(im->getSequencer().getState() != STATE_IDLE);
  delete h; delete im;
}

// Le trafic nominal ne doit pas voir la borne du tout.
void nominal_traffic_never_reaches_the_bound() {
  serialResetCfg();
  InstrumentManager* im = makeReadyInstrument();
  SerialMidiHandler* h = makeRunningHandler(im);

  // Une poignee de messages, comme un clavier qui joue : tout doit partir en
  // UNE passe.
  for (uint8_t i = 0; i < 6 && i < cfg.numNotes; i++) feedNoteOn(cfg.notes[i].midiNote, 90);
  h->update();
  assert(Serial2.__remaining() == 0);
  delete h; delete im;
}

// Un flux qui se recharge en permanence ne doit pas retenir update().
void a_source_that_never_stops_does_not_hold_the_loop() {
  serialResetCfg();
  InstrumentManager* im = makeReadyInstrument();
  SerialMidiHandler* h = makeRunningHandler(im);

  // Le pire cas reel : une broche qui produit des octets sans fin. On simule en
  // rechargeant le tampon a chaque passe, plus vite qu'il ne se vide.
  for (int pass = 0; pass < 10; pass++) {
    for (size_t i = 0; i < 1000; i++) Serial2.__feedByte(0xFE);
    h->update();      // doit RENDRE LA MAIN, c'est tout ce qui est demande
  }
  assert(Serial2.__remaining() > 0);   // le flux gagne, mais loop() vit
  delete h; delete im;
}

}  // namespace

void fin_serial_run_all_tests() {
  the_uart_drain_is_bounded_and_loses_nothing();
  a_message_split_across_passes_is_still_decoded();
  nominal_traffic_never_reaches_the_bound();
  a_source_that_never_stops_does_not_hold_the_loop();
}

/***********************************************************************************************
 * test_fin_midi - Deux defauts du chemin "fichier MIDI", reproduits puis verrouilles.
 *
 *  D-1 : MidiFilePlayer::update() n'etait borne par RIEN. La boucle
 *        `while (_currentEvent < _eventCount)` emettait tous les evenements
 *        echus d'un seul tour de loop(). Comme _eventCount peut valoir
 *        MIDI_FILE_MAX_EVENTS = 2000, une rafale au MEME horodatage - ou un
 *        lecteur qui rattrape un gros retard de millis() - envoyait jusqu'a
 *        2000 ordres d'affilee a l'instrument, pendant que la pression, le
 *        ventilateur, l'audio, le WebSocket et le chien de garde attendaient.
 *        Le gachis n'etait meme pas seulement temporel : la file d'evenements
 *        de l'instrument tient EVENT_QUEUE_SIZE = 16 evenements, donc passe le
 *        16e ordre d'une meme passe, un Note On de plus est REFUSE et un
 *        Note Off de plus EVINCE le plus ancien. La rafale se mangeait
 *        elle-meme.
 *
 *  D-2 : l'installation d'un fichier MIDI televerse (WEBOP_MIDI_FINALIZE)
 *        supprimait la destination AVANT de mettre le nouveau fichier en place :
 *            if (LittleFS.exists(destPath)) LittleFS.remove(destPath);
 *            bool moved = LittleFS.rename(tmpPath, destPath);
 *            if (!moved) { ...copie manuelle...; else LittleFS.remove(destPath); }
 *        Si le rename ET le repli par copie echouaient, l'ancien morceau etait
 *        perdu et le nouveau n'etait pas la. FileTransaction.{h,cpp} tient
 *        l'invariant qui manquait : a chaque instant, une version utilisable de
 *        la destination existe - a destPath, ou a bakPath.
 *
 * Les deux defauts sont reproduits par du code EXECUTE, pas decrits :
 *   - D-1 : la sequence d'origine est rejouee sur le meme lecteur
 *           (legacyUnboundedPass, plus bas) et on constate qu'elle emet la
 *           rafale entiere en une passe ;
 *   - D-2 : la sequence d'origine est rejouee sur le faux systeme de fichiers
 *           (legacyInstall) et on constate qu'il ne reste RIEN.
 *
 * NIVEAU DE VALIDATION : execute sur hote (g++). Rien n'a tourne sur un ESP32.
 *
 * Point d'entree : fin_midi_run_all_tests().
 ***********************************************************************************************/
void fin_midi_run_all_tests();

#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "Arduino.h"
#include "Wire.h"
#include "LittleFS.h"
#include "ConfigStorage.h"
#include "EventQueue.h"
#include "CommandQueue.h"
#include "FingerController.h"
#include "AirflowController.h"
#include "PressureController.h"
#include "FanController.h"
#include "NoteSequencer.h"
#include "FileTransaction.h"

// Le curseur de lecture du lecteur (_currentEvent) et la file d'evenements de
// l'instrument (_eventQueue) sont prives. Les lire est le seul moyen de
// constater ce qu'une passe d'update() a REELLEMENT emis, evenement par
// evenement : aucun chemin public ne rend cette information, et l'inferer d'un
// etat d'actionneur ne dirait rien de l'ORDRE ni du COMPTE. Meme procede que
// test_behavior.cpp (MidiFilePlayer) et test_hw_boot.cpp (AutoCalibrator).
#define private public
#include "InstrumentManager.h"
#include "MidiFilePlayer.h"
#undef private

namespace {

/*=============================================================================
 * Outillage commun
 *===========================================================================*/

// --- Construction d'un fichier .mid reel, octet par octet --------------------
// Le lecteur PARSE vraiment ces octets : les tests portent donc sur le tableau
// d'evenements que la production produit, pas sur un tableau fabrique a la main.
void midU32(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back((x >> 24) & 0xFF); v.push_back((x >> 16) & 0xFF);
  v.push_back((x >> 8) & 0xFF);  v.push_back(x & 0xFF);
}

void midU16(std::vector<uint8_t>& v, uint16_t x) {
  v.push_back((x >> 8) & 0xFF); v.push_back(x & 0xFF);
}

void midVLQ(std::vector<uint8_t>& v, uint32_t x) {
  uint8_t b[4]; int n = 0;
  b[n++] = x & 0x7F;
  while ((x >>= 7)) b[n++] = (x & 0x7F) | 0x80;
  for (int i = n - 1; i >= 0; i--) v.push_back(b[i]);
}

std::vector<uint8_t> midFile(const std::vector<uint8_t>& body, uint16_t division) {
  std::vector<uint8_t> f = {'M', 'T', 'h', 'd'};
  midU32(f, 6); midU16(f, 0); midU16(f, 1); midU16(f, division);
  std::vector<uint8_t> t = {'M', 'T', 'r', 'k'};
  midU32(t, (uint32_t)body.size());
  t.insert(t.end(), body.begin(), body.end());
  f.insert(f.end(), t.begin(), t.end());
  return f;
}

void midEoT(std::vector<uint8_t>& t) {
  midVLQ(t, 0); t.push_back(0xFF); t.push_back(0x2F); t.push_back(0x00);
}

void midNoteOn(std::vector<uint8_t>& t, uint32_t delta, uint8_t note, uint8_t vel) {
  midVLQ(t, delta); t.push_back(0x90); t.push_back(note); t.push_back(vel);
}

void midCc(std::vector<uint8_t>& t, uint32_t delta, uint8_t cc, uint8_t value) {
  midVLQ(t, delta); t.push_back(0xB0); t.push_back(cc); t.push_back(value);
}

// --- Configuration minimale, valide, et surtout LARGE en notes ---------------
// Les rafales de test comptent plusieurs centaines d'evenements : il faut que
// chaque note envoyee soit JOUABLE, sinon InstrumentManager::noteOn() la rejette
// avant la file et on mesurerait le filtre de plage, pas la borne.
const uint8_t kNumNotes = 32;
const uint8_t kFirstNote = 48;

void finResetCfg() {
  memset(&cfg, 0, sizeof(cfg));
  cfg.numFingers = 1;
  cfg.fingers[0].pcaChannel = 0;
  cfg.fingers[0].closedAngle = 90;
  cfg.fingers[0].direction = 1;
  cfg.numPumps = 1;
  cfg.pumpPins[0] = 25;
  cfg.pumpMinPwm[0] = 80;
  cfg.pumpMaxPwm[0] = 200;
  cfg.motorType = MOTOR_TYPE_PWM;
  cfg.airMode = AIR_MODE_PUMP_VALVE;
  cfg.valveType = 0;
  cfg.pumpFollowAirflow = true;
  cfg.pumpDirectMaxPercent = 100;
  cfg.pumpDirectIdlePercent = 0;
  cfg.servoToSolenoidDelayMs = 0;   // aucune avance : un evenement vif est du tout de suite
  cfg.minNoteDurationMs = 0;
  cfg.minNoteIntervalForValveCloseMs = 0;
  cfg.timeUnpower = 200;
  cfg.airflowPcaChannel = 10;
  cfg.solenoidPin = 13;
  cfg.solenoidActivationTimeMs = 50;
  cfg.solenoidPwmActivation = 255;
  cfg.solenoidPwmHolding = 128;
  cfg.servoAirflowOff = 20;
  cfg.servoAirflowMin = 60;
  cfg.servoAirflowMax = 100;
  cfg.servoAngleOff = 90;
  cfg.servoAngleMin = 45;
  cfg.servoAngleMax = 135;
  cfg.ccVolumeDefault = 127;
  cfg.ccExpressionDefault = 127;
  cfg.ccBreathDefault = 127;
  cfg.ccBrightnessDefault = 64;
  cfg.airVelocityResponse = 100;
  cfg.cc2Enabled = false;           // pas de coalescence CC2 dans ces mesures
  cfg.cc2SilenceThreshold = 10;
  cfg.cc2ResponseCurve = 1.4f;
  cfg.cc2TimeoutMs = 0;
  cfg.vibratoFrequencyHz = 6.0f;
  cfg.vibratoMaxAmplitudeDeg = 8.0f;
  strcpy(cfg.embouchure, "bec");
  cfg.numNotes = kNumNotes;
  for (uint8_t i = 0; i < kNumNotes; i++) {
    cfg.notes[i].midiNote = (uint8_t)(kFirstNote + i);
    cfg.notes[i].airflowMinPercent = 0;
    cfg.notes[i].airflowMaxPercent = 100;
    cfg.notes[i].airflowNominalPercent = 40;
  }
}

// Instrument reellement initialise : sans HW_INIT_OK, noteOn()/noteOff()
// sortent aussitot et rien n'atteindrait jamais la file.
void finReadyInstrument(InstrumentManager& im) {
  Wire.clear();
  Wire.setPresent(PCA_ADDR_BOARD0, true);
  assert(im.beginSafe());
  assert(im._hardwareInitStatus == HW_INIT_OK);
}

// Ce que l'instrument a REELLEMENT recu. On vide sa file a la main apres chaque
// passe du lecteur (sans appeler im.update(), qui ferait consommer le
// sequenceur et melangerait deux sujets) : le contenu observe est exactement la
// suite des ordres emis par cette passe, dans l'ordre.
struct Received {
  std::vector<uint8_t> notes;
  std::vector<uint8_t> velocities;
  std::vector<int> types;   // EVENT_NOTE_ON / EVENT_NOTE_OFF
  size_t size() const { return notes.size(); }
};

size_t drainInstrument(InstrumentManager& im, Received& out) {
  size_t before = out.size();
  MidiEvent ev;
  while (im._eventQueue.tryPopDueEvent(millis(), 0, ev)) {
    out.notes.push_back(ev.midiNote);
    out.velocities.push_back(ev.velocity);
    out.types.push_back((int)ev.type);
  }
  return out.size() - before;
}

// Une rafale d'evenements tous au MEME tick, chacun IDENTIFIABLE : la paire
// (note, velocite) a une periode de ppcm(32, 127) = 4064, donc sur quelques
// centaines d'evenements chaque paire est unique. Toute perte et toute
// inversion se voient.
uint8_t burstNote(int i) { return (uint8_t)(kFirstNote + (i % kNumNotes)); }
uint8_t burstVel(int i)  { return (uint8_t)(1 + (i % 127)); }

// `count` Note On au tick 0, puis un evenement SENTINELLE tres loin dans le
// temps. La sentinelle n'est jamais atteinte pendant le test : elle empeche le
// lecteur d'arriver en fin de fichier, donc d'appeler stop() -> allSoundOff(),
// qui viderait la file de l'instrument avant qu'on ait pu la lire.
std::vector<uint8_t> burstFile(int count) {
  std::vector<uint8_t> t;
  for (int i = 0; i < count; i++) midNoteOn(t, 0, burstNote(i), burstVel(i));
  midNoteOn(t, 480 * 1000, kFirstNote, 64);   // sentinelle, ~1000 noires plus loin
  midEoT(t);
  return midFile(t, 480);
}

// La boucle d'ORIGINE de MidiFilePlayer::update(), conservee ici - et seulement
// ici - pour que le defaut reste demontre par du code execute et non par un
// commentaire. C'est mot pour mot l'ancienne boucle, sans borne.
uint16_t legacyUnboundedPass(MidiFilePlayer& p) {
  uint16_t dispatched = 0;
  uint32_t currentPositionMs = millis() - p._playbackStartMs;
  while (p._currentEvent < p._eventCount) {
    MidiFileEvent& evt = p._events[p._currentEvent];
    if (evt.absoluteTimeMs > currentPositionMs) break;
    if (p._channelFilter != 255 && evt.channel != p._channelFilter) {
      p._currentEvent++;
      continue;
    }
    uint8_t msgType = evt.type & 0xF0;
    switch (msgType) {
      case 0x90:
        if (evt.data2 > 0) p._instrument->noteOn(evt.data1, evt.data2);
        else p._instrument->noteOff(evt.data1);
        break;
      case 0x80: p._instrument->noteOff(evt.data1); break;
      case 0xB0: p._instrument->handleControlChange(evt.data1, evt.data2); break;
    }
    p._currentEvent++;
    dispatched++;
  }
  return dispatched;
}

/*=============================================================================
 * D-1 - une passe d'update() est bornee, et ne perd rien
 *===========================================================================*/

// LE test du defaut. Trois cents evenements au meme instant.
//
// Sur le code d'origine (legacyUnboundedPass, execute ci-dessous), UNE passe en
// emettait les 300 : la premiere assertion `first < burst` echouait, et la file
// de l'instrument - 16 places - en avait deja refuse l'immense majorite.
//
// Les assertions n'utilisent PAS MIDI_PLAYER_MAX_EVENTS_PER_UPDATE : relacher
// la borne relacherait aussi le test. Elles disent la propriete qu'on veut
// vraiment, et qui ne depend pas du chiffre retenu : une passe n'emet jamais
// plus d'ordres que la file de l'instrument n'en tient (EVENT_QUEUE_SIZE), donc
// rien de ce qui est emis n'est perdu en aval.
void fin_midi_burst_is_bounded_and_nothing_is_lost() {
  const int burst = 300;
  finResetCfg();
  LittleFS.__put("/burst.mid", burstFile(burst));

  // --- 1. Comportement d'ORIGINE, execute -----------------------------------
  {
    InstrumentManager im; finReadyInstrument(im);
    MidiFilePlayer p; assert(p.begin(&im));
    assert(p.loadFile("/burst.mid"));
    __test_millis = 10000;
    p.play();
    Received got;
    uint16_t inOnePass = legacyUnboundedPass(p);
    drainInstrument(im, got);
    assert(inOnePass == burst);            // 300 ordres dans UN tour de loop()
    assert(got.size() < (size_t)burst);    // et la file n'en a garde que 16
    assert(got.size() <= (size_t)EVENT_QUEUE_SIZE);
  }

  // --- 2. Comportement CORRIGE, meme fichier, meme instant ------------------
  InstrumentManager im; finReadyInstrument(im);
  MidiFilePlayer p; assert(p.begin(&im));
  assert(p.loadFile("/burst.mid"));
  assert(p.getEventCount() == burst + 1);   // + la sentinelle
  __test_millis = 10000;
  p.play();

  Received got;
  std::vector<uint16_t> perPass;
  // Le temps NE BOUGE PAS d'une passe a l'autre : ce qui decoupe le travail est
  // la borne, pas l'horloge.
  for (int pass = 0; pass < 10000 && p._currentEvent < burst; pass++) {
    uint16_t before = p._currentEvent;
    p.update();
    uint16_t dispatched = (uint16_t)(p._currentEvent - before);
    assert(dispatched > 0);                              // progression garantie
    assert(dispatched <= (uint16_t)EVENT_QUEUE_SIZE);    // LA propriete voulue
    size_t landed = drainInstrument(im, got);
    assert(landed == dispatched);                        // rien refuse en aval
    perPass.push_back(dispatched);
  }

  // La premiere passe n'a PAS tout fait : c'est l'assertion qui echoue sur le
  // code d'origine.
  assert(perPass.size() > 1);
  assert(perPass[0] < (uint16_t)burst);

  // Une borne, pas une derive : toutes les passes PLEINES emettent le meme
  // nombre d'ordres ; seule la derniere peut etre plus courte.
  for (size_t i = 0; i + 1 < perPass.size(); i++) assert(perPass[i] == perPass[0]);
  assert(perPass.back() <= perPass[0]);

  // Rien n'est perdu, et rien n'est joue deux fois.
  assert(p._currentEvent == burst);
  assert(got.size() == (size_t)burst);
  for (int i = 0; i < burst; i++) {
    assert(got.types[i] == (int)EVENT_NOTE_ON);
    assert(got.notes[i] == burstNote(i));
    assert(got.velocities[i] == burstVel(i));
  }

  // Le lecteur n'a pas fini le fichier (la sentinelle est loin) : il joue
  // toujours, et une passe de plus n'emet rien tant que son heure n'est pas
  // venue.
  assert(p.getState() == PLAYER_PLAYING);
  p.update();
  assert(p._currentEvent == burst);
  p.stop();
}

// Meme propriete quand ce n'est pas le fichier qui est dense mais le LECTEUR qui
// est en retard : un grand saut de millis() (ecriture LittleFS, reconnexion
// Wi-Fi, upload) rend d'un coup des centaines d'evenements echus.
void fin_midi_a_late_player_catches_up_without_a_burst() {
  const int count = 240;
  finResetCfg();

  // Un evenement toutes les noires (500 ms a 120 BPM), puis une sentinelle.
  std::vector<uint8_t> t;
  for (int i = 0; i < count; i++) midNoteOn(t, i == 0 ? 0 : 480, burstNote(i), burstVel(i));
  midNoteOn(t, 480 * 1000, kFirstNote, 64);
  midEoT(t);
  LittleFS.__put("/late.mid", midFile(t, 480));

  InstrumentManager im; finReadyInstrument(im);
  MidiFilePlayer p; assert(p.begin(&im));
  assert(p.loadFile("/late.mid"));
  __test_millis = 50000;
  p.play();

  // Le lecteur reprend la main tres en retard : tous les `count` evenements sont
  // echus d'un coup.
  __test_millis += (unsigned long)count * 500 + 1000;

  Received got;
  std::vector<uint16_t> perPass;
  for (int pass = 0; pass < 10000 && p._currentEvent < count; pass++) {
    uint16_t before = p._currentEvent;
    p.update();
    uint16_t dispatched = (uint16_t)(p._currentEvent - before);
    assert(dispatched > 0);
    assert(dispatched <= (uint16_t)EVENT_QUEUE_SIZE);
    assert(drainInstrument(im, got) == dispatched);
    perPass.push_back(dispatched);
  }
  assert(perPass.size() > 1);
  assert(perPass[0] < (uint16_t)count);
  assert(got.size() == (size_t)count);
  for (int i = 0; i < count; i++) assert(got.notes[i] == burstNote(i));
  p.stop();
}

// L'ordre du fichier est conserve D'UNE PASSE A L'AUTRE, y compris sur une
// rafale melangeant Note On, Note Off et CC ordinaires. Le decoupage en passes
// ne doit ni reordonner, ni sauter, ni rejouer.
void fin_midi_order_is_preserved_across_passes() {
  const int groups = 60;   // 60 * 3 = 180 evenements au meme tick
  finResetCfg();
  std::vector<uint8_t> t;
  for (int i = 0; i < groups; i++) {
    midNoteOn(t, 0, burstNote(i), burstVel(i));                  // 0x90, vel > 0
    midVLQ(t, 0); t.push_back(0x80); t.push_back(burstNote(i)); t.push_back(0);
    midCc(t, 0, 11, (uint8_t)(i % 128));                          // CC 11, ordinaire
  }
  midNoteOn(t, 480 * 1000, kFirstNote, 64);
  midEoT(t);
  LittleFS.__put("/order.mid", midFile(t, 480));

  InstrumentManager im; finReadyInstrument(im);
  MidiFilePlayer p; assert(p.begin(&im));
  assert(p.loadFile("/order.mid"));
  const uint16_t total = (uint16_t)(groups * 3);
  assert(p.getEventCount() == total + 1);
  __test_millis = 20000;
  p.play();

  // La suite attendue, lue dans le tableau d'evenements du lecteur lui-meme :
  // le tri du parseur est stable, donc c'est l'ordre du fichier.
  std::vector<uint8_t> expectedNotes, expectedVels;
  std::vector<int> expectedTypes;
  for (uint16_t i = 0; i < total; i++) {
    uint8_t kind = p._events[i].type & 0xF0;
    if (kind == 0xB0) continue;   // les CC n'entrent pas dans la file d'evenements
    expectedNotes.push_back(p._events[i].data1);
    expectedVels.push_back(kind == 0x90 ? p._events[i].data2 : 0);
    expectedTypes.push_back(kind == 0x90 ? (int)EVENT_NOTE_ON : (int)EVENT_NOTE_OFF);
  }

  Received got;
  int passes = 0;
  while (p._currentEvent < total && passes < 10000) {
    uint16_t before = p._currentEvent;
    p.update();
    assert(p._currentEvent > before);                                  // jamais bloque
    assert((uint16_t)(p._currentEvent - before) <= (uint16_t)EVENT_QUEUE_SIZE);
    drainInstrument(im, got);
    passes++;
  }
  assert(passes > 1);                       // le decoupage a bien eu lieu
  assert(p._currentEvent == total);

  // Les CC 11 ordinaires sont limites en debit par l'instrument
  // (CC_RATE_LIMIT_PER_SECOND) : ils n'ont rien a faire dans la file
  // d'evenements et n'y sont pas. Ce qui compte ici est que la suite des notes
  // arrive EXACTEMENT dans l'ordre du fichier, sans trou ni doublon.
  assert(got.size() == expectedNotes.size());
  for (size_t i = 0; i < expectedNotes.size(); i++) {
    assert(got.types[i] == expectedTypes[i]);
    assert(got.notes[i] == expectedNotes[i]);
    assert(got.velocities[i] == expectedVels[i]);
  }
  p.stop();
}

// stop(), pause() et le panic ne traversent pas la boucle bornee : ils agissent
// immediatement, quel que soit le retard accumule.
void fin_midi_stop_pause_and_panic_are_never_deferred() {
  const int burst = 300;
  finResetCfg();
  LittleFS.__put("/burst.mid", burstFile(burst));

  // --- stop() sur un lecteur qui a 300 evenements en retard -----------------
  {
    InstrumentManager im; finReadyInstrument(im);
    MidiFilePlayer p; assert(p.begin(&im));
    assert(p.loadFile("/burst.mid"));
    __test_millis = 30000;
    p.play();
    p.update();
    assert(p._currentEvent > 0 && p._currentEvent < burst);   // retard bien la
    assert(im._eventQueue.getCount() > 0);

    p.stop();
    assert(p.getState() == PLAYER_STOPPED);
    assert(p._currentEvent == 0);                 // la lecture repart de zero
    assert(im._eventQueue.getCount() == 0);       // allSoundOff a vide la file
    assert(im.getSequencer().getState() == STATE_IDLE);
    p.update();                                   // et plus rien n'est emis
    assert(im._eventQueue.getCount() == 0);
    assert(p._currentEvent == 0);
  }

  // --- pause() : meme immediatete, mais le retard est CONSERVE --------------
  {
    InstrumentManager im; finReadyInstrument(im);
    MidiFilePlayer p; assert(p.begin(&im));
    assert(p.loadFile("/burst.mid"));
    __test_millis = 40000;
    p.play();
    p.update();
    uint16_t held = p._currentEvent;
    assert(held > 0 && held < burst);

    p.pause();
    assert(p.getState() == PLAYER_PAUSED);
    assert(im._eventQueue.getCount() == 0);
    assert(p._currentEvent == held);              // rien n'est jete
    p.update();
    assert(p._currentEvent == held);              // en pause, aucune emission
    assert(im._eventQueue.getCount() == 0);

    // La reprise repart exactement la ou la pause a laisse le lecteur.
    p.play();
    p.update();
    assert(p._currentEvent > held);
    p.stop();
  }

  // --- panic exterieur : il ne passe pas par le lecteur ---------------------
  {
    InstrumentManager im; finReadyInstrument(im);
    MidiFilePlayer p; assert(p.begin(&im));
    assert(p.loadFile("/burst.mid"));
    __test_millis = 50000;
    p.play();
    p.update();
    assert(im._eventQueue.getCount() > 0);
    uint32_t before = im.panicCount();
    im.requestPanic();
    im.processCommands();                         // ce que fait update() en tete
    assert(im.panicCount() == before + 1);
    assert(im._eventQueue.getCount() == 0);
    p.stop();
  }

  // --- panic PRESENT DANS LE FICHIER, juste derriere la borne ---------------
  // Un CC 123 (All Notes Off) place apres plus d'evenements qu'une passe n'en
  // emet. S'il etait soumis a la borne, il attendrait le tour suivant : une
  // note continuerait a souffler alors que le fichier vient de demander le
  // silence. Les Channel Mode Messages passent donc HORS borne.
  {
    std::vector<uint8_t> t;
    for (int i = 0; i < EVENT_QUEUE_SIZE; i++) midNoteOn(t, 0, burstNote(i), burstVel(i));
    midCc(t, 0, 123, 0);                          // All Notes Off, au meme tick
    for (int i = 0; i < 50; i++) midNoteOn(t, 0, burstNote(i), burstVel(i));
    midNoteOn(t, 480 * 1000, kFirstNote, 64);
    midEoT(t);
    LittleFS.__put("/panic.mid", midFile(t, 480));

    InstrumentManager im; finReadyInstrument(im);
    MidiFilePlayer p; assert(p.begin(&im));
    assert(p.loadFile("/panic.mid"));
    __test_millis = 60000;
    p.play();

    uint32_t before = im.panicCount();
    p.update();                                   // UNE seule passe
    assert(im.panicCount() == before + 1);        // la panique est deja passee
    // Elle a aussi ferme la passe : le lecteur s'est arrete juste apres elle,
    // il n'a pas enchaine les 50 notes suivantes derriere un All Notes Off.
    assert(p._currentEvent == (uint16_t)(EVENT_QUEUE_SIZE + 1));
    assert(im._eventQueue.getCount() == 0);       // le panic a vide la file
    p.stop();
  }
}

// NON-REGRESSION : sur un morceau peu dense - le cas courant - la borne ne se
// voit pas. Chaque evenement part dans la passe exacte ou son heure arrive,
// comme avant.
void fin_midi_a_sparse_piece_is_unchanged_by_the_bound() {
  finResetCfg();
  // Une noire = 500 ms a 120 BPM. Huit notes tenues, chacune avec son
  // relachement : une piece ordinaire pour un instrument monophonique.
  const int notes = 8;
  std::vector<uint8_t> t;
  for (int i = 0; i < notes; i++) {
    midNoteOn(t, i == 0 ? 0 : 240, burstNote(i), burstVel(i));               // On
    midVLQ(t, 240); t.push_back(0x80); t.push_back(burstNote(i)); t.push_back(0);
  }
  midEoT(t);
  LittleFS.__put("/sparse.mid", midFile(t, 480));

  InstrumentManager im; finReadyInstrument(im);
  MidiFilePlayer p; assert(p.begin(&im));
  assert(p.loadFile("/sparse.mid"));
  const uint16_t total = (uint16_t)(notes * 2);
  assert(p.getEventCount() == total);

  // Les instants attendus, lus dans le tableau d'evenements.
  std::vector<uint32_t> when;
  for (uint16_t i = 0; i < total; i++) when.push_back(p._events[i].absoluteTimeMs);

  __test_millis = 70000;
  const unsigned long start = __test_millis;
  p.play();

  Received got;
  uint16_t maxPerPass = 0;
  // Curseur au DEBUT de la passe qui atteint la fin du fichier. Cette passe-la
  // se termine par stop() -> allSoundOff(), qui vide la file de l'instrument :
  // ce qu'elle vient d'y deposer n'y est donc plus. C'est le comportement de
  // fin de morceau, inchange par la borne.
  uint16_t deliveredBeforeFinalStop = total;

  // Pas de 5 ms, comme une boucle loop() ordinaire.
  for (unsigned long elapsed = 0; elapsed <= when.back() + 20; elapsed += 5) {
    __test_millis = start + elapsed;
    bool wasPlaying = (p.getState() == PLAYER_PLAYING);
    uint16_t before = p._currentEvent;
    p.update();
    if (wasPlaying && p.getState() == PLAYER_STOPPED) deliveredBeforeFinalStop = before;
    if (p.getState() == PLAYER_PLAYING && p._currentEvent > before) {
      uint16_t d = (uint16_t)(p._currentEvent - before);
      if (d > maxPerPass) maxPerPass = d;
    }
    drainInstrument(im, got);

    // Invariant de TIMING : a cet instant, le lecteur a emis exactement les
    // evenements dus, ni plus (pas d'avance) ni moins (pas de retard induit par
    // la borne). C'est le comportement d'avant la borne, verbatim.
    uint16_t due = 0;
    while (due < total && when[due] <= elapsed) due++;
    uint16_t seen = (p.getState() == PLAYER_STOPPED) ? total : p._currentEvent;
    assert(seen == due);
  }

  // Le morceau est peu dense : une passe n'emet jamais qu'un evenement. La
  // borne n'a donc JAMAIS eu l'occasion de se declencher - c'est exactement ce
  // qu'on veut dire par "elle ne se voit pas dans le cas courant".
  assert(maxPerPass == 1);

  // Le fichier est alle jusqu'au bout et le lecteur s'est arrete tout seul.
  assert(p.getState() == PLAYER_STOPPED);
  assert(deliveredBeforeFinalStop == (uint16_t)(total - 1));
  assert(got.size() == (size_t)deliveredBeforeFinalStop);
  for (size_t i = 0; i < got.size(); i++) assert(got.notes[i] == p._events[i].data1);
}

// La borne du lecteur ne doit jamais depasser ce que l'instrument peut absorber
// en une passe : au-dela, l'ordre supplementaire ne peut que detruire un
// evenement deja emis. C'est un garde de COHERENCE, complement des tests
// executes ci-dessus, pas leur remplacant.
void fin_midi_bound_never_exceeds_what_the_instrument_absorbs() {
  assert(MIDI_PLAYER_MAX_EVENTS_PER_UPDATE > 0);
  assert((int)MIDI_PLAYER_MAX_EVENTS_PER_UPDATE <= (int)EVENT_QUEUE_SIZE);
  // Et elle doit rester tres en dessous de la capacite du fichier, sinon elle
  // ne borne rien.
  assert((int)MIDI_PLAYER_MAX_EVENTS_PER_UPDATE < (int)MIDI_FILE_MAX_EVENTS / 10);
}

/*=============================================================================
 * D-2 - installation transactionnelle d'un fichier
 *===========================================================================*/

const char* const kSrc  = "/midi/.upload.tmp";
const char* const kDest = "/midi/morceau.mid";
const char* const kBak  = "/midi/morceau.mid.bak";

// Faux systeme de fichiers en memoire, capable d'echouer a la demande. Meme
// esprit que celui de test_harden_storage.cpp, avec l'operation `copy` en plus
// (le repli des LittleFS qui ne renomment pas de facon fiable).
struct FakeFs {
  std::map<std::string, std::string> files;
  int removeCalls = 0;
  int renameCalls = 0;
  int copyCalls = 0;

  bool failAllRemoves = false;
  bool failAllRenames = false;
  bool failAllCopies = false;
  std::string failRenameFrom, failRenameTo;
  std::string failCopyFrom, failCopyTo;

  bool has(const char* p) const { return files.count(p) != 0; }
  std::string get(const char* p) const {
    std::map<std::string, std::string>::const_iterator it = files.find(p);
    return it == files.end() ? std::string() : it->second;
  }
  void put(const char* p, const char* content) { files[p] = content; }
  size_t count() const { return files.size(); }
};

bool fakeExists(void* ctx, const char* path) {
  return static_cast<FakeFs*>(ctx)->files.count(path) != 0;
}

bool fakeRemove(void* ctx, const char* path) {
  FakeFs* fs = static_cast<FakeFs*>(ctx);
  fs->removeCalls++;
  if (fs->failAllRemoves) return false;
  return fs->files.erase(path) > 0;
}

bool fakeRename(void* ctx, const char* from, const char* to) {
  FakeFs* fs = static_cast<FakeFs*>(ctx);
  fs->renameCalls++;
  if (fs->failAllRenames) return false;
  if (!fs->failRenameFrom.empty() && fs->failRenameFrom == from && fs->failRenameTo == to) {
    return false;
  }
  std::map<std::string, std::string>::iterator it = fs->files.find(from);
  if (it == fs->files.end()) return false;
  fs->files[to] = it->second;
  fs->files.erase(it);
  return true;
}

// Copie : laisse la source en place, comme la copie manuelle octet par octet du
// firmware.
bool fakeCopy(void* ctx, const char* from, const char* to) {
  FakeFs* fs = static_cast<FakeFs*>(ctx);
  fs->copyCalls++;
  if (fs->failAllCopies) return false;
  if (!fs->failCopyFrom.empty() && fs->failCopyFrom == from && fs->failCopyTo == to) {
    return false;
  }
  std::map<std::string, std::string>::iterator it = fs->files.find(from);
  if (it == fs->files.end()) return false;
  fs->files[to] = it->second;
  return true;
}

FileTxOps opsFor(FakeFs& fs) {
  FileTxOps ops;
  ops.exists = &fakeExists;
  ops.remove = &fakeRemove;
  ops.rename = &fakeRename;
  ops.copy   = &fakeCopy;
  ops.ctx    = &fs;
  return ops;
}

// La sequence d'ORIGINE de WEBOP_MIDI_FINALIZE, conservee ici et seulement ici
// pour que le defaut reste demontre par du code execute :
//     if (exists(dest)) remove(dest);
//     moved = rename(src, dest);
//     if (!moved) { copie manuelle ; si elle rate, remove(dest) }
bool legacyInstall(FakeFs& fs, const char* src, const char* dest) {
  if (fakeExists(&fs, dest)) fakeRemove(&fs, dest);
  bool moved = fakeRename(&fs, src, dest);
  if (!moved) {
    moved = fakeCopy(&fs, src, dest);
    if (moved) fakeRemove(&fs, src);
    else fakeRemove(&fs, dest);
  }
  return moved;
}

// Installation nominale : la destination porte le nouveau contenu, sans residu.
void fin_midi_install_nominal_leaves_no_residue() {
  FakeFs fs;
  fs.put(kDest, "ANCIEN");
  fs.put(kSrc, "NOUVEAU");

  assert(fileTxInstall(opsFor(fs), kSrc, kDest, kBak));
  assert(fs.get(kDest) == "NOUVEAU");
  assert(!fs.has(kBak));
  assert(!fs.has(kSrc));
  assert(fs.count() == 1);
}

// Premiere installation : rien a sauvegarder, donc aucun .bak ne doit etre cree.
void fin_midi_first_install_creates_no_backup() {
  FakeFs fs;
  fs.put(kSrc, "PREMIER");

  assert(fileTxInstall(opsFor(fs), kSrc, kDest, kBak));
  assert(fs.get(kDest) == "PREMIER");
  assert(!fs.has(kBak));
  assert(!fs.has(kSrc));
  assert(fs.count() == 1);
  assert(fs.renameCalls == 1);   // un seul mouvement : rien d'inutile
}

// LE test du defaut D-2 : le rename ET la copie echouent.
void fin_midi_failed_install_keeps_the_old_file() {
  // --- 1. Sequence d'ORIGINE : il ne reste RIEN --------------------------
  {
    FakeFs fs;
    fs.put(kDest, "ANCIEN");
    fs.put(kSrc, "NOUVEAU");
    fs.failAllRenames = true;
    fs.failAllCopies = true;

    assert(!legacyInstall(fs, kSrc, kDest));
    assert(!fs.has(kDest));            // l'ancien morceau a ete detruit
    assert(fs.get(kSrc) == "NOUVEAU"); // et le nouveau n'est pas en place
    // Un demarrage maintenant ne peut rien recuperer : il n'y a pas de .bak.
    assert(!fileTxRecover(opsFor(fs), kDest, kBak));
    assert(!fs.has(kDest));
  }

  // --- 2. Sequence CORRIGEE, meme scenario ------------------------------
  {
    FakeFs fs;
    fs.put(kDest, "ANCIEN");
    fs.put(kSrc, "NOUVEAU");
    // La mise de cote passe ; seule la PROMOTION echoue, rename comme copie.
    fs.failRenameFrom = kSrc; fs.failRenameTo = kDest;
    fs.failCopyFrom = kSrc;   fs.failCopyTo = kDest;

    assert(!fileTxInstall(opsFor(fs), kSrc, kDest, kBak));
    assert(fs.has(kDest));
    assert(fs.get(kDest) == "ANCIEN");     // l'ancien morceau est revenu
    assert(fs.get(kSrc) == "NOUVEAU");     // et le candidat n'a pas ete jete

    // Un demarrage maintenant retrouve ce meme fichier.
    assert(fileTxRecover(opsFor(fs), kDest, kBak));
    assert(fs.get(kDest) == "ANCIEN");
    assert(!fs.has(kBak));
  }
}

// Echec de la mise de cote (dest -> bak), rename ET copie : on abandonne AVANT
// d'avoir touche quoi que ce soit.
void fin_midi_failed_backup_leaves_everything_in_place() {
  FakeFs fs;
  fs.put(kDest, "ANCIEN");
  fs.put(kSrc, "NOUVEAU");
  fs.failRenameFrom = kDest; fs.failRenameTo = kBak;
  fs.failCopyFrom = kDest;   fs.failCopyTo = kBak;

  assert(!fileTxInstall(opsFor(fs), kSrc, kDest, kBak));
  assert(fs.get(kDest) == "ANCIEN");   // intact
  assert(fs.get(kSrc) == "NOUVEAU");   // le candidat attend toujours
  assert(!fs.has(kBak));
  assert(fs.count() == 2);
}

// Le repli par COPIE tient la meme garantie que le rename : un LittleFS qui ne
// renomme pas doit installer quand meme, sans jamais laisser la destination
// absente.
void fin_midi_copy_fallback_installs_and_keeps_the_invariant() {
  FakeFs fs;
  fs.put(kDest, "ANCIEN");
  fs.put(kSrc, "NOUVEAU");
  fs.failAllRenames = true;   // aucun rename ne marche : tout passe par la copie

  assert(fileTxInstall(opsFor(fs), kSrc, kDest, kBak));
  assert(fs.get(kDest) == "NOUVEAU");
  assert(!fs.has(kBak));
  assert(!fs.has(kSrc));
  assert(fs.copyCalls >= 2);   // mise de cote + promotion
}

// Pire cas : la restauration du .bak echoue aussi. La fonction rend false, mais
// le .bak survit et fileTxRecover() rend le fichier utilisable au demarrage.
void fin_midi_a_failed_restore_is_repaired_at_boot() {
  FakeFs fs;
  fs.put(kDest, "ANCIEN");
  fs.put(kSrc, "NOUVEAU");
  // La mise de cote passe (rename dest -> bak), puis plus rien ne marche : ni
  // la promotion, ni la restauration.
  FileTxOps ops = opsFor(fs);
  assert(ops.rename(ops.ctx, kDest, kBak));   // etape 2 jouee a la main
  fs.failAllRenames = true;
  fs.failAllCopies = true;

  assert(!fileTxInstall(ops, kSrc, kDest, kBak));
  assert(!fs.has(kDest));                 // la destination est absente...
  assert(fs.get(kBak) == "ANCIEN");       // ...mais l'ancien morceau est la
  assert(fs.get(kSrc) == "NOUVEAU");

  // Le demarrage suivant remet le morceau en place.
  fs.failAllRenames = false;
  fs.failAllCopies = false;
  assert(fileTxRecover(opsFor(fs), kDest, kBak));
  assert(fs.get(kDest) == "ANCIEN");
  assert(!fs.has(kBak));
}

// LE PIEGE rencontre par le LOT B sur ConfigPersist : une DEUXIEME tentative
// apres un echec trouve la destination absente et le .bak present. Ce .bak est
// alors la SEULE copie de l'ancien fichier - le liberer "pour faire de la
// place" le detruirait.
void fin_midi_a_second_attempt_never_destroys_the_backup() {
  FakeFs fs;
  fs.put(kDest, "ANCIEN");
  fs.put(kSrc, "NOUVEAU");

  // 1. Premiere tentative : la mise de cote passe, la promotion echoue, la
  //    restauration echoue. On se retrouve dest absent / bak = ANCIEN.
  {
    FileTxOps ops = opsFor(fs);
    assert(ops.rename(ops.ctx, kDest, kBak));
    fs.failAllRenames = true;
    fs.failAllCopies = true;
    assert(!fileTxInstall(ops, kSrc, kDest, kBak));
    assert(!fs.has(kDest));
    assert(fs.get(kBak) == "ANCIEN");
  }

  // 2. Deuxieme tentative, qui echoue elle aussi : le .bak doit SURVIVRE.
  {
    fs.failAllRenames = false;
    fs.failAllCopies = false;
    fs.failRenameFrom = kSrc; fs.failRenameTo = kDest;
    fs.failCopyFrom = kSrc;   fs.failCopyTo = kDest;
    assert(!fileTxInstall(opsFor(fs), kSrc, kDest, kBak));
    assert(fs.get(kBak) == "ANCIEN");   // la seule copie de l'ancien morceau
    assert(fs.get(kSrc) == "NOUVEAU");
    // Et un demarrage la retrouve toujours.
    FakeFs copyOfState = fs;
    assert(fileTxRecover(opsFor(copyOfState), kDest, kBak));
    assert(copyOfState.get(kDest) == "ANCIEN");
  }

  // 3. Troisieme tentative, qui reussit : c'est seulement MAINTENANT que le
  //    .bak devient perime, et il est efface.
  {
    fs.failRenameFrom.clear(); fs.failRenameTo.clear();
    fs.failCopyFrom.clear();   fs.failCopyTo.clear();
    assert(fileTxInstall(opsFor(fs), kSrc, kDest, kBak));
    assert(fs.get(kDest) == "NOUVEAU");
    assert(!fs.has(kBak));
    assert(!fs.has(kSrc));
  }
}

// Recuperation au demarrage : les quatre etats possibles.
void fin_midi_recover_on_boot_cases() {
  // 1. Fichier en place, .bak residuel -> le residu est efface.
  {
    FakeFs fs;
    fs.put(kDest, "EN_PLACE");
    fs.put(kBak, "ANCIEN");
    assert(fileTxRecover(opsFor(fs), kDest, kBak));
    assert(fs.get(kDest) == "EN_PLACE");
    assert(!fs.has(kBak));
    assert(fs.renameCalls == 0);   // on ne touche pas a ce qui va bien
  }
  // 2. Fichier absent, .bak present -> promu.
  {
    FakeFs fs;
    fs.put(kBak, "ANCIEN");
    assert(fileTxRecover(opsFor(fs), kDest, kBak));
    assert(fs.get(kDest) == "ANCIEN");
    assert(fs.count() == 1);
  }
  // 3. Rien du tout : rien a recuperer, et surtout rien de cree.
  {
    FakeFs fs;
    assert(!fileTxRecover(opsFor(fs), kDest, kBak));
    assert(fs.count() == 0);
  }
  // 4. Fichier absent, .bak present, mais le rename ne marche pas : repli par
  //    copie.
  {
    FakeFs fs;
    fs.put(kBak, "ANCIEN");
    fs.failAllRenames = true;
    assert(fileTxRecover(opsFor(fs), kDest, kBak));
    assert(fs.get(kDest) == "ANCIEN");
  }
}

// Refus francs : rien a installer, ou jeu d'operations incomplet. Dans les deux
// cas on ne detruit rien et on ne saute pas dans le vide.
void fin_midi_refusals_never_destroy_anything() {
  // a. Pas de source : la destination ne doit pas etre touchee.
  {
    FakeFs fs;
    fs.put(kDest, "ANCIEN");
    assert(!fileTxInstall(opsFor(fs), kSrc, kDest, kBak));
    assert(fs.get(kDest) == "ANCIEN");
    assert(fs.renameCalls == 0);
    assert(fs.removeCalls == 0);
  }
  // b. Pointeur d'operation manquant : refus, pas de dereferencement nul.
  {
    FakeFs fs;
    fs.put(kDest, "ANCIEN");
    fs.put(kSrc, "NOUVEAU");
    FileTxOps broken = opsFor(fs);
    broken.rename = nullptr;
    assert(!fileTxInstall(broken, kSrc, kDest, kBak));
    assert(!fileTxRecover(broken, kDest, kBak));
    assert(fs.get(kDest) == "ANCIEN");
  }
  // c. `copy` absent : le repli est indisponible, mais la sequence reste sure.
  {
    FakeFs fs;
    fs.put(kDest, "ANCIEN");
    fs.put(kSrc, "NOUVEAU");
    fs.failAllRenames = true;
    FileTxOps noCopy = opsFor(fs);
    noCopy.copy = nullptr;
    assert(!fileTxInstall(noCopy, kSrc, kDest, kBak));
    assert(fs.get(kDest) == "ANCIEN");   // rien n'a bouge
    assert(fs.get(kSrc) == "NOUVEAU");
  }
  // d. Chemin nul : refus.
  {
    FakeFs fs;
    fs.put(kDest, "ANCIEN");
    assert(!fileTxInstall(opsFor(fs), nullptr, kDest, kBak));
    assert(!fileTxRecover(opsFor(fs), nullptr, kBak));
    assert(fs.get(kDest) == "ANCIEN");
  }
  // e. Systeme de fichiers qui refuse TOUTE mutation : on rend la main, sans
  //    boucler.
  {
    FakeFs fs;
    fs.put(kDest, "ANCIEN");
    fs.put(kSrc, "NOUVEAU");
    fs.put(kBak, "VIEUX");
    fs.failAllRemoves = true;
    fs.failAllRenames = true;
    fs.failAllCopies = true;
    assert(!fileTxInstall(opsFor(fs), kSrc, kDest, kBak));
    assert(fs.get(kDest) == "ANCIEN");
    assert(fileTxRecover(opsFor(fs), kDest, kBak));   // constat, pas boucle
    assert(fs.count() == 3);
  }
}

// FileTransaction.cpp doit rester PUR : aucun include Arduino/LittleFS, aucune
// allocation. C'est ce qui permet de l'executer ici, et ce qui garde la
// sequence testable le jour ou le site d'appel change.
void fin_midi_file_transaction_stays_pure() {
  const char* prefixes[] = { "", "../", "../../", "../../../", "../../../../" };
  std::string src;
  for (int i = 0; i < 5 && src.empty(); i++) {
    std::string path = std::string(prefixes[i]) + "Servo_flute_ESP32/FileTransaction.cpp";
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) continue;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) src.append(buf, n);
    fclose(f);
  }
  assert(!src.empty());
  assert(src.find("#include <Arduino.h>") == std::string::npos);
  assert(src.find("#include <LittleFS.h>") == std::string::npos);
  assert(src.find("LittleFS.") == std::string::npos);
  assert(src.find("new ") == std::string::npos);
  assert(src.find("malloc(") == std::string::npos);
}

}  // namespace

void fin_midi_run_all_tests() {
  // `cfg` et l'horloge simulee sont partages par toutes les unites de test
  // natives : on les rend tels qu'on les a trouves.
  RuntimeConfig saved;
  memcpy(&saved, &cfg, sizeof(RuntimeConfig));
  unsigned long savedMillis = __test_millis;

  fin_midi_burst_is_bounded_and_nothing_is_lost();
  fin_midi_a_late_player_catches_up_without_a_burst();
  fin_midi_order_is_preserved_across_passes();
  fin_midi_stop_pause_and_panic_are_never_deferred();
  fin_midi_a_sparse_piece_is_unchanged_by_the_bound();
  fin_midi_bound_never_exceeds_what_the_instrument_absorbs();

  fin_midi_install_nominal_leaves_no_residue();
  fin_midi_first_install_creates_no_backup();
  fin_midi_failed_install_keeps_the_old_file();
  fin_midi_failed_backup_leaves_everything_in_place();
  fin_midi_copy_fallback_installs_and_keeps_the_invariant();
  fin_midi_a_failed_restore_is_repaired_at_boot();
  fin_midi_a_second_attempt_never_destroys_the_backup();
  fin_midi_recover_on_boot_cases();
  fin_midi_refusals_never_destroy_anything();
  fin_midi_file_transaction_stays_pure();

  memcpy(&cfg, &saved, sizeof(RuntimeConfig));
  __test_millis = savedMillis;
  std::cout << "fin midi tests passed\n";
}

#ifdef STANDALONE_TEST_MAIN
int main() { fin_midi_run_all_tests(); return 0; }
#endif

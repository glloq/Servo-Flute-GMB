// Tests de surete materielle : plafond de duree de note, et compteur de
// paniques.
//
// Les deux defauts verrouilles ici ont ete reproduits en executant le code de
// production, pas deduits de sa lecture :
//
//   1. AUCUN plafond de duree de note n'existait. Un Note Off qui n'arrive
//      jamais - MIDI DIN debranche, source qui n'emet pas d'Active Sensing,
//      donc aucune detection de perte de lien possible cote SerialMidiHandler -
//      laissait la valve ouverte, la bobine a son PWM de maintien et la pompe
//      en regime indefiniment :
//          t = 240 ms   OE=LOW  sol=128  pompe=173  valve=OUVERTE  seq=PLAYING
//          t = 10 min   OE=LOW  sol=128  pompe=173  valve=OUVERTE  seq=PLAYING
//      cfg.timeUnpower ne pouvait rien : il ne coupe l'OE que lorsque le
//      sequenceur est DEJA au repos.
//
//   2. Une panique venue d'un transport MIDI n'etait visible d'aucun
//      observateur exterieur. requestCalibrationCancel() n'existe que dans
//      WebConfigurator et n'est appelee que par les chemins WEB ; la
//      deconnexion BLE, la deconnexion rtpMIDI, la chute du lien Wi-Fi STA, le
//      timeout Active Sensing du MIDI serie et les CC120/123 ne l'appellent
//      pas. La calibration repartait donc apres la mise en securite :
//          calibration en cours : valve=OUVERTE sol=255 running=1
//          juste apres le panic : valve=fermee  sol=0   running=1
//          740 ms plus tard     : valve REOUVERTE par le calibrateur
//      InstrumentManager::panicCount() est la moitie basse de la correction :
//      il rend la panique observable quel que soit le chemin qui l'a
//      declenchee. Ces tests verrouillent ce que le compteur compte et, tout
//      aussi important, ce qu'il ne compte PAS.
#include <cassert>
#include <cstring>
#include <iostream>
#include <map>
#include "Arduino.h"
#include "Wire.h"
#include "ConfigStorage.h"
#include "EventQueue.h"
#include "FingerController.h"
#include "AirflowController.h"
#include "NoteSequencer.h"
#include "InstrumentManager.h"

extern std::map<uint8_t, int> __analog_writes, __digital_writes;

namespace {

// Configuration minimale et valide, commune a ce fichier : un doigt, une pompe
// directe, une valve a solenoide. Les valeurs de temps sont volontairement
// courtes (positionnement 10 ms) pour que seul le plafond gouverne la duree.
void hwResetCfg() {
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
  cfg.valveType = 0;                      // solenoide
  cfg.pumpFollowAirflow = true;
  cfg.pumpDirectMaxPercent = 100;
  cfg.pumpDirectIdlePercent = 0;          // valeur d'usine : entre deux notes, air coupe
  cfg.servoToSolenoidDelayMs = 10;
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
  cfg.cc2Enabled = true;
  cfg.cc2SilenceThreshold = 10;
  cfg.cc2ResponseCurve = 1.4f;
  cfg.cc2TimeoutMs = 0;                   // 0 = pas d'expiration CC2 : elle couperait la note
  cfg.vibratoFrequencyHz = 6.0f;
  cfg.vibratoMaxAmplitudeDeg = 8.0f;
  strcpy(cfg.embouchure, "bec");
  cfg.numNotes = 3;
  cfg.notes[0].midiNote = 60;
  cfg.notes[1].midiNote = 61;
  cfg.notes[2].midiNote = 62;
  for (int i = 0; i < 3; i++) {
    cfg.notes[i].airflowMinPercent = 0;
    cfg.notes[i].airflowMaxPercent = 100;
    cfg.notes[i].airflowNominalPercent = 40;
  }
}

InstrumentManager* makeReadyHwInstrument() {
  Wire.clear();
  Wire.setPresent(PCA_ADDR_BOARD0, true);
  InstrumentManager* im = new InstrumentManager();
  assert(im->beginSafe());
  assert(im->isHardwareReady());
  return im;
}

// Avance le temps par pas de `stepMs` jusqu'a `untilMs` en appelant update() a
// chaque pas : c'est la boucle loop() du firmware, pas un saut de temps unique.
// Un saut unique masquerait un plafond qui ne se declencherait qu'au premier
// tour suivant.
void runUntil(InstrumentManager* im, unsigned long untilMs, unsigned long stepMs) {
  while (__test_millis < untilMs) {
    unsigned long next = __test_millis + stepMs;
    __test_millis = (next > untilMs) ? untilMs : next;
    im->update();
  }
}

/*=============================================================================
 * DEFAUT 1 - plafond de duree de note
 *===========================================================================*/

// Le scenario du defaut, execute de bout en bout : Note On, puis plus jamais
// rien. Sans plafond, cette note tient les actionneurs jusqu'au redemarrage.
//
// Ce test verifie les TROIS etages de l'extinction demandes, pas seulement
// l'etat du sequenceur : la valve (PWM de la bobine remis a zero), l'air (la
// consigne de pompe revenue a son repos) et l'alimentation servo (l'OE coupe
// par cfg.timeUnpower, qui ne pouvait jamais s'appliquer pendant une note
// tenue - c'est precisement ce que le plafond debloque).
void hold_ceiling_cuts_a_stuck_note_completely() {
  hwResetCfg();
  InstrumentManager* im = makeReadyHwInstrument();
  __analog_writes.clear();
  __test_millis = 1000;

  im->noteOn(60, 120);
  im->update();                                   // t=1000 : POSITIONING
  assert(im->getSequencer().getState() == STATE_POSITIONING);

  __test_millis = 1010;
  im->update();                                   // la valve s'ouvre : la note SONNE
  const unsigned long soundStart = 1010;
  assert(im->getSequencer().getState() == STATE_PLAYING);
  assert(im->getAirflowCtrl().isValveOpen());
  assert(im->getPressureCtrl().getTargetPercent() > cfg.pumpDirectIdlePercent);

  // Le releve du defaut, reproduit : la bobine est bien retombee a son PWM de
  // maintien (le firmware fait son travail thermique) et l'OE est toujours a
  // LOW. C'est l'etat ou la machine restait indefiniment.
  runUntil(im, soundStart + 240, 20);
  assert(__analog_writes[cfg.solenoidPin] == cfg.solenoidPwmHolding);
  assert(__digital_writes[PIN_SERVOS_OFF] == LOW);
  assert(im->getSequencer().getState() == STATE_PLAYING);

  // UNE MILLISECONDE AVANT LE PLAFOND, rien n'a change. Cette borne-ci est ce
  // qui empeche le plafond de devenir un coupe-note zele : un plafond qui
  // mordrait plus tot casserait la musique.
  runUntil(im, soundStart + NOTE_HOLD_CEILING_MS - 1, 500);
  assert(im->getSequencer().getState() == STATE_PLAYING);
  assert(im->getAirflowCtrl().isValveOpen());
  assert(im->getAirflowCtrl().isNoteSounding());
  assert(im->getPressureCtrl().getTargetPercent() > cfg.pumpDirectIdlePercent);

  // LE PLAFOND. Extinction complete au tour suivant.
  __test_millis = soundStart + NOTE_HOLD_CEILING_MS;
  im->update();
  assert(im->getSequencer().getState() == STATE_IDLE);
  assert(!im->getAirflowCtrl().isValveOpen());
  assert(!im->getAirflowCtrl().isNoteActive());
  assert(__analog_writes[cfg.solenoidPin] == 0);                       // bobine relachee
  assert(im->getPressureCtrl().getTargetPercent() == cfg.pumpDirectIdlePercent);  // air coupe

  // Et le retour a STATE_IDLE rend enfin effectif cfg.timeUnpower : l'OE
  // remonte, donc les servos de doigts et de souffle ne sont plus alimentes.
  // Cet etage-la etait INATTEIGNABLE avant la correction.
  runUntil(im, __test_millis + cfg.timeUnpower, 50);
  assert(__digital_writes[PIN_SERVOS_OFF] == HIGH);

  // Le plafond TIENT : les tours suivants ne relancent rien.
  runUntil(im, __test_millis + 5000, 250);
  assert(im->getSequencer().getState() == STATE_IDLE);
  assert(!im->getAirflowCtrl().isValveOpen());
  assert(__digital_writes[PIN_SERVOS_OFF] == HIGH);

  // Et l'instrument n'est pas mort : une VRAIE note suivante rejoue.
  im->noteOn(61, 100);
  runUntil(im, __test_millis + 40, 10);
  assert(im->getSequencer().getState() == STATE_PLAYING);
  assert(im->getAirflowCtrl().isValveOpen());

  im->allSoundOff();
  delete im;
}

// La raison pour laquelle le plafond passe par NoteSequencer::stop() et non par
// l'arret de note ordinaire.
//
// stopCurrentNote() consulte shouldCloseValveBetweenNotes(), qui LAISSE LA
// VALVE OUVERTE quand un Note On attend a moins de
// cfg.minNoteIntervalForValveCloseMs - c'est l'optimisation de legato, et elle
// est juste en temps normal. Mais quand le plafond est atteint, plus rien ne
// prouve que le lien qui a depose ces evenements est encore vivant : leur faire
// confiance rouvrirait la valve dans la foulee et le plafond serait defait au
// tour suivant, exactement comme la panique defaite par le calibrateur.
void hold_ceiling_is_not_undone_by_a_queued_note() {
  hwResetCfg();
  cfg.minNoteIntervalForValveCloseMs = 200;   // fenetre de legato large
  __test_millis = 0;

  FingerController fc([](uint8_t, uint16_t, uint16_t) {});
  AirflowController ac([](uint8_t, uint16_t, uint16_t) {});
  EventQueue q(16);
  NoteSequencer ns(q, fc, ac);
  ns.begin();

  assert(q.enqueueScheduledEvent(EVENT_NOTE_ON, 60, 100, 0));
  ns.update();
  assert(ns.getState() == STATE_POSITIONING);

  __test_millis = 10;
  ns.update();
  assert(ns.getState() == STATE_PLAYING && ac.isValveOpen());
  const unsigned long soundStart = 10;

  // Un Note On programme JUSTE APRES l'instant du plafond : il est encore dans
  // la file au moment ou le plafond mord (son echeance avancee du delai de
  // positionnement tombe a 30090), et il est assez proche pour que
  // shouldCloseValveBetweenNotes() reponde "garde la valve ouverte".
  const unsigned long ceilingAt = soundStart + NOTE_HOLD_CEILING_MS;
  __test_millis = ceilingAt - 10;
  assert(q.enqueueScheduledEvent(EVENT_NOTE_ON, 62, 100, ceilingAt + 90));
  ns.update();
  assert(ns.getState() == STATE_PLAYING && ac.isValveOpen());   // pas encore au plafond
  assert(!q.isEmpty());

  // Le plafond : la valve est fermee SANS CONDITION, et la file est videe pour
  // que rien du contexte d'avant ne la rouvre.
  __test_millis = ceilingAt;
  ns.update();
  assert(ns.getState() == STATE_IDLE);
  assert(!ac.isValveOpen());
  assert(q.isEmpty());

  // La note qui attendait ne joue jamais : c'est cela, "le plafond tient".
  __test_millis = ceilingAt + 200;
  ns.update();
  assert(ns.getState() == STATE_IDLE);
  assert(!ac.isValveOpen());
  assert(ns.getCurrentNote() == 0);
}

// Ce que le plafond doit LAISSER PASSER. 24 s de note tenue, c'est deja au-dela
// de tout ce qui se note (une ronde a 40 BPM dure 6 s) et au-dela de ce qu'un
// souffleur humain tient sur une expiration. Cette note-la doit s'arreter sur
// SON Note Off, pas sur le plafond.
void a_long_musical_note_is_not_cut() {
  hwResetCfg();
  InstrumentManager* im = makeReadyHwInstrument();
  __test_millis = 5000;

  im->noteOn(60, 110);
  im->update();
  __test_millis = 5010;
  im->update();
  const unsigned long soundStart = 5010;
  assert(im->getSequencer().getState() == STATE_PLAYING);

  // 24 s de tenue : le plafond ne doit intervenir a aucun tour.
  runUntil(im, soundStart + 24000, 100);
  assert(im->getSequencer().getState() == STATE_PLAYING);
  assert(im->getAirflowCtrl().isValveOpen());
  assert(im->getAirflowCtrl().isNoteSounding());
  assert(im->getPressureCtrl().getTargetPercent() > cfg.pumpDirectIdlePercent);

  // Son propre Note Off l'arrete, par le chemin ordinaire.
  im->noteOff(60);
  runUntil(im, __test_millis + 40, 10);
  assert(im->getSequencer().getState() == STATE_IDLE);
  assert(!im->getAirflowCtrl().isValveOpen());

  im->allSoundOff();
  delete im;
}

/*=============================================================================
 * DEFAUT 2 - panicCount(), la moitie basse du contrat
 *===========================================================================*/

// Toute panique REELLEMENT executee se compte, quel que soit le transport qui
// l'a declenchee - et une seule fois par panique, pas une fois par demande.
void panic_count_rises_once_per_panic_whatever_the_path() {
  hwResetCfg();
  InstrumentManager* im = makeReadyHwInstrument();
  __test_millis = 1000;
  assert(im->panicCount() == 0);   // rien au demarrage

  // 1. Perte de transport : deconnexion BLE, deconnexion rtpMIDI, chute du lien
  //    Wi-Fi STA, timeout Active Sensing du MIDI serie. Aucun de ces chemins ne
  //    traverse le moindre code web - c'est le trou que le compteur bouche.
  im->handleTransportLost();
  assert(im->panicCount() == 1);

  // 2. Panique POSTEE depuis une autre tache (callback NimBLE, WebSocket). Trois
  //    demandes avant que loop() ne reprenne la main : CommandQueue les
  //    coalesce, et UNE SEULE panique est executee, donc comptee.
  im->requestPanic();
  im->requestPanic();
  im->requestPanic();
  assert(im->panicCount() == 1);   // rien n'est compte a la DEMANDE
  __test_millis += 20;
  im->update();
  assert(im->panicCount() == 2);
  __test_millis += 20;
  im->update();
  assert(im->panicCount() == 2);   // pas de seconde execution fantome

  // 3. CC de mode canal recus sur la tache loop() : ce sont les commandes
  //    d'arret de la norme MIDI et elles arrivent par n'importe quel transport.
  im->handleControlChange(MIDI_CC_ALL_SOUND_OFF, 0);       // 120
  assert(im->panicCount() == 3);
  im->handleControlChange(MIDI_CC_ALL_NOTES_OFF, 0);       // 123
  assert(im->panicCount() == 4);
  im->handleControlChange(MIDI_CC_OMNI_OFF, 0);            // 124
  assert(im->panicCount() == 5);

  // 4. Le meme CC POSTE depuis une autre tache : il devient une demande de
  //    panique et se compte a sa consommation, une fois.
  assert(im->postCommand(ACMD_CONTROL_CHANGE, MIDI_CC_ALL_SOUND_OFF, 0));
  assert(im->panicCount() == 5);
  __test_millis += 20;
  im->update();
  assert(im->panicCount() == 6);

  delete im;
}

// Ce que le compteur ne compte PAS, et c'est aussi important : allSoundOff()
// est aussi le "mettre en securite" ordinaire du firmware. Le lecteur MIDI
// l'appelle sur pause et en fin de morceau ; le serveur web l'appelle avant un
// redemarrage controle (reset, reset usine, formatage). Aucun de ces cas n'est
// une panique et aucun ne doit faire annuler une calibration.
void panic_count_ignores_the_ordinary_safing_paths() {
  hwResetCfg();
  InstrumentManager* im = makeReadyHwInstrument();
  __test_millis = 1000;

  im->handleTransportLost();
  const uint32_t afterRealPanic = im->panicCount();
  assert(afterRealPanic == 1);

  // Le lecteur MIDI en pause / en fin de morceau, et le serveur web avant un
  // reboot : appel DIRECT a allSoundOff().
  im->allSoundOff();
  im->allSoundOff();
  assert(im->panicCount() == afterRealPanic);

  // CC121 Reset All Controllers : remise a zero des controleurs, pas une panique.
  im->handleControlChange(MIDI_CC_RESET_ALL_CONTROLLERS, 0);
  im->resetAllControllers();
  assert(im->panicCount() == afterRealPanic);

  // Prise de possession des actionneurs par le calibrateur : elle arrete bien la
  // note en cours (_sequencer.stop()), mais ce n'est pas une panique - sinon le
  // calibrateur s'annulerait lui-meme au demarrage.
  im->setActuatorSessionActive(true);
  im->setActuatorSessionActive(false);
  assert(im->panicCount() == afterRealPanic);

  // Une note ordinaire, jouee et relachee, ne compte evidemment rien.
  im->noteOn(60, 100);
  runUntil(im, __test_millis + 60, 10);
  im->noteOff(60);
  runUntil(im, __test_millis + 60, 10);
  assert(im->panicCount() == afterRealPanic);

  // Et une note coupee par le PLAFOND de duree n'est pas une panique non plus :
  // c'est une fin de note, automatique et locale au sequenceur. Le calibrateur
  // ne tourne de toute facon jamais en meme temps que le sequenceur.
  im->noteOn(61, 100);
  runUntil(im, __test_millis + 20, 10);
  assert(im->getSequencer().getState() == STATE_PLAYING);
  runUntil(im, __test_millis + NOTE_HOLD_CEILING_MS + 10, 500);
  assert(im->getSequencer().getState() == STATE_IDLE);
  assert(im->panicCount() == afterRealPanic);

  delete im;
}

// La panique doit rester visible MEME pendant qu'une session d'actionneurs
// (auto-calibration) possede le materiel : c'est le seul moment ou le compteur
// sert reellement a quelque chose. Si le chemin de panique etait avale par la
// session, le consommateur n'aurait rien a voir et le defaut resterait entier.
void panic_is_counted_even_while_the_calibrator_owns_the_actuators() {
  hwResetCfg();
  InstrumentManager* im = makeReadyHwInstrument();
  __test_millis = 1000;

  im->setActuatorSessionActive(true);
  assert(im->isActuatorSessionActive());
  const uint32_t before = im->panicCount();

  // Chute du lien Wi-Fi STA / deconnexion rtpMIDI pendant la calibration : le
  // pire cas cite par l'audit, parce que la calibration avait ete lancee depuis
  // le navigateur et que plus personne ne regarde.
  im->handleTransportLost();
  assert(im->panicCount() == before + 1);

  // CC123 recu par un transport MIDI pendant la calibration : traite avant tout
  // filtrage de session, donc compte lui aussi.
  im->handleControlChange(MIDI_CC_ALL_NOTES_OFF, 0);
  assert(im->panicCount() == before + 2);

  // Panique postee depuis une autre tache : consommee par update() meme pendant
  // la session.
  im->requestPanic();
  __test_millis += 20;
  im->update();
  assert(im->panicCount() == before + 3);

  im->setActuatorSessionActive(false);
  delete im;
}

// Contrat de lecture : monotone croissant, jamais remis a zero. Un consommateur
// memorise la derniere valeur vue et compare par INEGALITE - c'est ce qui rend
// le compteur correct meme si uint32_t debordait (4 milliards de paniques :
// sans objet, mais la comparaison ne suppose aucun ordre).
void panic_count_is_monotonic_and_never_resets() {
  hwResetCfg();
  InstrumentManager* im = makeReadyHwInstrument();
  __test_millis = 1000;

  uint32_t lastSeen = im->panicCount();   // ce que ferait le consommateur
  bool sawPanic = false;

  for (int i = 0; i < 5; i++) {
    const uint32_t previous = im->panicCount();
    im->handleTransportLost();
    assert(im->panicCount() >= previous);   // jamais decroissant
    // La comparaison du consommateur : une INEGALITE, pas un ordre.
    if (im->panicCount() != lastSeen) {
      sawPanic = true;
      lastSeen = im->panicCount();
    }
  }
  assert(sawPanic && lastSeen == 5);

  // Une re-initialisation hardware ne remet PAS le compteur a zero : un
  // consommateur qui avait memorise 5 verrait sinon "0 != 5" et conclurait a une
  // panique imaginaire, ou pire, manquerait les suivantes.
  assert(im->beginSafe());
  assert(im->panicCount() == 5);
  im->handleTransportLost();
  assert(im->panicCount() == 6);

  delete im;
}

}  // namespace

void hw_note_run_all_tests() {
  hold_ceiling_cuts_a_stuck_note_completely();
  hold_ceiling_is_not_undone_by_a_queued_note();
  a_long_musical_note_is_not_cut();
  panic_count_rises_once_per_panic_whatever_the_path();
  panic_count_ignores_the_ordinary_safing_paths();
  panic_is_counted_even_while_the_calibrator_owns_the_actuators();
  panic_count_is_monotonic_and_never_resets();
}

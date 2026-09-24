// Durcissement des trois taches FreeRTOS : contrat des demandes inter-taches,
// borne du travail par passe, et survie a une allocation refusee.
//
// CE QUE CES TESTS PROUVENT, ET CE QU'ILS NE PROUVENT PAS
// -------------------------------------------------------
// Trois taches coexistent sur la carte : loop(), la tache AsyncTCP (HTTP /
// WebSocket) et la tache hote NimBLE. Un test sur HOTE est MONO-TACHE : il ne
// peut PAS reproduire l'entrelacement de deux taches FreeRTOS, et rien ici n'a
// tourne sur un ESP32. Ce que ces tests verrouillent est le CONTRAT que le code
// de production doit offrir pour que la course soit impossible :
//
//   - une demande se PREND (lecture + effacement) en un seul geste, donc une
//     demande deposee apres la prise ne peut plus etre effacee par erreur ;
//   - deux demandes deposees avant la reprise de loop() se coalescent en une,
//     et JAMAIS en zero ;
//   - le travail applique par passe est BORNE, le reste est DIFFERE (jamais
//     perdu), le panic n'est jamais retarde par la borne, et un relachement
//     n'est jamais reporte indefiniment ;
//   - une file dont l'allocation a echoue refuse proprement au lieu de
//     dereferencer un pointeur nul, et les deux chemins non perdables (panic,
//     Note Off) continuent de fonctionner puisqu'ils ne vivent pas dans le
//     tableau alloue.
//
// L'absence de course reelle entre les taches n'est PAS demontree ici : elle
// decoule de l'atomicite des sections critiques (portENTER_CRITICAL), qui sont
// des no-ops sur l'hote.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include "Arduino.h"
#include "Wire.h"
#include "ConfigStorage.h"
#include "CommandQueue.h"
#include "EventQueue.h"
#include "InstrumentManager.h"

extern std::map<uint8_t, int> __analog_writes, __digital_writes;

namespace {

// Configuration minimale et valide : un doigt, une pompe directe, une valve a
// solenoide. Identique d'esprit a celle des autres fichiers hw_*, avec des
// temps courts pour que les passes d'update() soient lisibles.
void tasksResetCfg() {
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
  cfg.pumpDirectIdlePercent = 0;
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
  cfg.ccVolumeDefault = 100;              // != des valeurs postees par les tests
  cfg.ccExpressionDefault = 127;
  cfg.ccBreathDefault = 127;
  cfg.ccBrightnessDefault = 64;
  cfg.airVelocityResponse = 100;
  cfg.cc2Enabled = true;
  cfg.cc2SilenceThreshold = 10;
  cfg.cc2ResponseCurve = 1.4f;
  cfg.cc2TimeoutMs = 0;
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

InstrumentManager* makeReadyInstrument() {
  Wire.clear();
  Wire.setPresent(PCA_ADDR_BOARD0, true);
  InstrumentManager* im = new InstrumentManager();
  assert(im->beginSafe());
  assert(im->isHardwareReady());
  return im;
}

// Remplit l'anneau de commandes jusqu'a saturation avec des consignes de pompe
// de valeurs CROISSANTES : getTargetPercent() rend alors la valeur de la
// DERNIERE commande appliquee, donc le NOMBRE de commandes appliquees et leur
// ORDRE sont directement observables.
uint8_t fillRingWithPumpTargets(InstrumentManager* im, uint8_t firstValue, uint8_t howMany) {
  uint8_t accepted = 0;
  for (uint8_t i = 0; i < howMany; i++) {
    if (!im->postCommand(ACMD_PUMP_TARGET, 0, (uint8_t)(firstValue + i))) break;
    accepted++;
  }
  return accepted;
}

/*=============================================================================
 * C-1 - une demande inter-taches se prend en UN seul geste
 *
 * Le defaut : `volatile bool` + la sequence "si le drapeau est pose, l'effacer"
 * ne sont PAS atomiques. volatile interdit seulement la mise en cache par le
 * compilateur - ni atomicite, ni barriere. Une demande deposee par la tache
 * AsyncTCP entre la lecture et l'effacement etait effacee sans avoir ete
 * traitee : un CC121 "Reset All Controllers" disparaissait, ou les servos
 * restaient non alimentes.
 *
 * Ce qui est verifiable sur hote : l'existence et le comportement d'une prise
 * unique (take) qui lit ET efface, donc l'IMPOSSIBILITE de la sequence en deux
 * temps. La course elle-meme n'est pas rejouee ici (voir l'en-tete du fichier).
 *===========================================================================*/

void reset_controllers_request_is_taken_and_cleared_in_one_go() {
  tasksResetCfg();
  InstrumentManager* im = makeReadyInstrument();

  // Etat de depart : aucune demande.
  assert(!im->resetControllersRequestPending());
  assert(!im->takeResetControllersRequest());

  // La tache AsyncTCP poste un CC121. Il ne passe pas par l'anneau (il ne doit
  // pas pouvoir etre perdu par saturation), mais par le drapeau dedie.
  assert(im->postCommand(ACMD_CONTROL_CHANGE, MIDI_CC_RESET_ALL_CONTROLLERS, 0));
  assert(im->resetControllersRequestPending());

  // La prise lit ET efface : une seule prise rend true.
  assert(im->takeResetControllersRequest());
  assert(!im->resetControllersRequestPending());
  assert(!im->takeResetControllersRequest());

  // COALESCENCE : deux demandes avant que loop() ne reprenne la main = un seul
  // traitement. Jamais zero.
  assert(im->postCommand(ACMD_CONTROL_CHANGE, MIDI_CC_RESET_ALL_CONTROLLERS, 0));
  assert(im->postCommand(ACMD_CONTROL_CHANGE, MIDI_CC_RESET_ALL_CONTROLLERS, 0));
  assert(im->takeResetControllersRequest());
  assert(!im->takeResetControllersRequest());

  // LE CAS QUI COMPTE : la demande suivante est deposee APRES la prise, pendant
  // que la precedente est encore en cours de traitement (resetAllControllers()
  // dure : il touche les controleurs). Elle doit survivre au traitement en
  // cours et etre vue par la prise suivante.
  assert(im->postCommand(ACMD_CONTROL_CHANGE, MIDI_CC_RESET_ALL_CONTROLLERS, 0));
  assert(im->takeResetControllersRequest());          // loop() prend la demande...
  assert(im->postCommand(ACMD_CONTROL_CHANGE, MIDI_CC_RESET_ALL_CONTROLLERS, 0));
  im->resetAllControllers();                          // ...et la traite ENSUITE
  assert(im->resetControllersRequestPending());       // la nouvelle n'a PAS ete effacee
  assert(im->takeResetControllersRequest());
  assert(!im->takeResetControllersRequest());

  // Bout en bout par update() : la demande est reellement honoree, et une
  // deuxieme demande postee juste apres la passe l'est a la passe suivante.
  im->handleControlChange(MIDI_CC_VOLUME, 10);
  assert(im->getCCVolume() == 10);
  assert(im->postCommand(ACMD_CONTROL_CHANGE, MIDI_CC_RESET_ALL_CONTROLLERS, 0));
  __test_millis += 10;
  im->update();
  assert(im->getCCVolume() == cfg.ccVolumeDefault);
  assert(!im->resetControllersRequestPending());

  im->handleControlChange(MIDI_CC_VOLUME, 10);
  assert(im->getCCVolume() == 10);
  assert(im->postCommand(ACMD_CONTROL_CHANGE, MIDI_CC_RESET_ALL_CONTROLLERS, 0));
  __test_millis += 10;
  im->update();
  assert(im->getCCVolume() == cfg.ccVolumeDefault);

  delete im;
}

void power_on_request_is_taken_and_cleared_in_one_go() {
  tasksResetCfg();
  InstrumentManager* im = makeReadyInstrument();
  __test_millis = 1000;

  // beginSafe() a deja enregistre de l'activite : partir d'un etat propre.
  im->takePowerOnRequest();
  assert(!im->powerOnRequestPending());
  assert(!im->takePowerOnRequest());

  // registerActuatorActivity() est le producteur : il est appele depuis
  // N'IMPORTE QUELLE tache (enfilement d'une note, ecriture PWM) et ne touche
  // jamais le GPIO d'OE lui-meme.
  im->registerActuatorActivity();
  assert(im->powerOnRequestPending());
  assert(im->takePowerOnRequest());
  assert(!im->powerOnRequestPending());
  assert(!im->takePowerOnRequest());

  // Coalescence, jamais de perte.
  im->registerActuatorActivity();
  im->registerActuatorActivity();
  assert(im->takePowerOnRequest());
  assert(!im->takePowerOnRequest());

  // Le cas qui compte : demande deposee pendant que la precedente est traitee
  // (ensureServosPowered() ecrit le GPIO d'OE, ce n'est pas instantane).
  im->registerActuatorActivity();
  assert(im->takePowerOnRequest());
  im->registerActuatorActivity();            // <-- la tache AsyncTCP depose ICI
  im->ensureServosPowered();                 // fin du traitement de la premiere
  assert(im->powerOnRequestPending());       // la nouvelle demande a survecu
  assert(im->takePowerOnRequest());
  assert(!im->takePowerOnRequest());

  // Bout en bout : une fois les servos coupes au repos, une demande venue d'une
  // autre tache les REALIMENTE a la passe suivante - c'est exactement ce qu'une
  // demande perdue empechait (actionneurs laisses non alimentes).
  im->takePowerOnRequest();
  __test_millis += cfg.timeUnpower + 10;
  im->update();
  assert(__digital_writes[PIN_SERVOS_OFF] == HIGH);   // OE coupe, servos au repos

  im->registerActuatorActivity();                     // demande hors loop()
  assert(im->powerOnRequestPending());
  __test_millis += 10;
  im->update();
  assert(__digital_writes[PIN_SERVOS_OFF] == LOW);    // reelement realimentes

  delete im;
}

/*=============================================================================
 * C-2 - le travail applique par passe est borne
 *
 * Le defaut : processCommands() drainait la file ENTIEREMENT a chaque update().
 * Chaque applyCommand() peut emettre des transactions I2C vers les PCA9685 ;
 * sous saturation (client WebSocket en rafale), une seule passe executait donc
 * des dizaines d'ecritures d'affilee et affamait le reste de loop().
 *===========================================================================*/

void command_work_is_bounded_per_update_and_nothing_is_lost() {
  tasksResetCfg();
  InstrumentManager* im = makeReadyInstrument();
  __test_millis = 1000;
  im->getPressureCtrl().setTargetPercent(0);

  // Saturer l'anneau avec des consignes de valeurs 1, 2, 3, ... : la derniere
  // valeur appliquee dit combien de commandes ont ete appliquees, et dans quel
  // ordre.
  const uint8_t posted = fillRingWithPumpTargets(im, 1, COMMAND_QUEUE_SIZE);
  assert(posted == COMMAND_QUEUE_SIZE);
  assert(im->commandQueue().count() == COMMAND_QUEUE_SIZE);
  assert(im->commandQueue().droppedCount() == 0);

  // UNE passe : au plus la borne, et le reste est TOUJOURS EN FILE.
  __test_millis += 10;
  im->update();
  const uint8_t appliedFirstPass = im->getPressureCtrl().getTargetPercent();
  assert(appliedFirstPass >= 1);                                   // du travail a ete fait
  assert(appliedFirstPass <= INSTRUMENT_MAX_COMMANDS_PER_UPDATE);  // mais BORNE
  // La MEME propriete, exprimee SANS la constante : une file saturee ne se vide
  // pas en une seule passe. Sans cette assertion, relacher la borne relacherait
  // aussi le test qui la surveille.
  assert(appliedFirstPass < COMMAND_QUEUE_SIZE);
  assert(im->commandQueue().count() == (uint8_t)(COMMAND_QUEUE_SIZE - appliedFirstPass));
  assert(im->commandQueue().droppedCount() == 0);                  // DIFFEREES, pas perdues

  // Les passes suivantes finissent le travail, dans l'ORDRE d'emission et sans
  // jamais depasser la borne.
  uint8_t previous = appliedFirstPass;
  int passes = 0;
  while (im->commandQueue().count() > 0 && passes < 64) {
    __test_millis += 10;
    im->update();
    passes++;
    const uint8_t now = im->getPressureCtrl().getTargetPercent();
    assert(now >= previous);                                          // jamais de retour en arriere
    assert((uint8_t)(now - previous) <= INSTRUMENT_MAX_COMMANDS_PER_UPDATE);
    previous = now;
  }
  assert(im->commandQueue().count() == 0);
  // TOUTES appliquees, la derniere en dernier : aucune commande perdue par la borne.
  assert(im->getPressureCtrl().getTargetPercent() == COMMAND_QUEUE_SIZE);
  assert(im->commandQueue().droppedCount() == 0);
  // Pas de famine : la file pleine s'est videe en un nombre de passes fini et
  // previsible (ceil(capacite / borne)).
  assert(passes <= (COMMAND_QUEUE_SIZE + INSTRUMENT_MAX_COMMANDS_PER_UPDATE - 1) /
                       INSTRUMENT_MAX_COMMANDS_PER_UPDATE);

  delete im;
}

void panic_is_executed_in_the_saturated_pass() {
  tasksResetCfg();
  InstrumentManager* im = makeReadyInstrument();
  __test_millis = 1000;

  // Anneau sature, puis une passe pour se placer AU MILIEU du drainage borne.
  assert(fillRingWithPumpTargets(im, 1, COMMAND_QUEUE_SIZE) == COMMAND_QUEUE_SIZE);
  __test_millis += 10;
  im->update();
  assert(im->commandQueue().count() > 0);        // il reste du travail differe

  // La tache NimBLE (perte de transport) demande un panic. Il ne doit PAS
  // attendre que les commandes differees soient ecoulees.
  const uint32_t before = im->panicCount();
  im->requestPanic();
  __test_millis += 10;
  im->update();                                   // UNE seule passe
  assert(im->panicCount() == before + 1);         // execute MAINTENANT
  assert(!im->commandQueue().panicPending());
  assert(im->commandQueue().count() == 0);        // et les commandes anterieures abandonnees
  assert(im->getPressureCtrl().getTargetPercent() == 0);   // allSoundOff() a bien tourne

  delete im;
}

void pending_note_offs_are_never_lost_and_keep_the_ring_first_order() {
  tasksResetCfg();
  InstrumentManager* im = makeReadyInstrument();
  __test_millis = 1000;

  // Une note joue reellement.
  im->noteOn(60, 100);
  for (int i = 0; i < 4; i++) { __test_millis += 20; im->update(); }
  assert(im->getSequencer().getState() == STATE_PLAYING);
  assert(im->getAirflowCtrl().isValveOpen());

  // Anneau sature, puis le relachement : il n'emprunte pas l'anneau (bitmap).
  assert(fillRingWithPumpTargets(im, 1, COMMAND_QUEUE_SIZE) == COMMAND_QUEUE_SIZE);
  assert(im->postCommand(ACMD_NOTE_OFF, 60));
  assert(im->commandQueue().hasPendingNoteOff());

  // ORDRE DOCUMENTE : l'anneau d'abord. Tant qu'il reste des commandes
  // differees, le relachement attend - un Note On deja en file doit etre
  // applique AVANT le Note Off qui le suit, sinon la note reste bloquee.
  for (uint8_t pass = 0; pass < INSTRUMENT_MAX_NOTE_OFF_DEFERRAL_PASSES; pass++) {
    __test_millis += 10;
    im->update();
    assert(im->commandQueue().count() > 0);
    assert(im->commandQueue().hasPendingNoteOff());   // encore differe
  }

  // ...mais JAMAIS indefiniment : passe la borne de report, le relachement est
  // applique meme si l'anneau n'est pas vide. Une note bloquee (valve et
  // souffle ouverts) est pire qu'une note perdue.
  __test_millis += 10;
  im->update();
  assert(!im->commandQueue().hasPendingNoteOff());
  assert(im->commandQueue().count() > 0);             // l'anneau n'etait PAS vide

  // Le relachement a reellement eteint la note.
  for (int i = 0; i < 6; i++) { __test_millis += 20; im->update(); }
  assert(!im->getAirflowCtrl().isValveOpen());
  assert(im->getSequencer().getState() == STATE_IDLE);

  // Et l'anneau finit par se vider : rien n'a ete perdu au passage.
  for (int i = 0; i < 16 && im->commandQueue().count() > 0; i++) {
    __test_millis += 10;
    im->update();
  }
  assert(im->commandQueue().count() == 0);
  assert(im->commandQueue().droppedCount() == 0);

  delete im;
}

void a_burst_of_note_offs_is_bounded_per_pass_and_fully_applied() {
  tasksResetCfg();
  InstrumentManager* im = makeReadyInstrument();
  __test_millis = 1000;

  // Anneau PLEIN. C'est desormais la SEULE facon de faire tomber un Note Off
  // dans le bitmap : postCommand() l'envoie d'abord dans l'anneau, et ne bascule
  // sur le bitmap qu'au refus. Avec un anneau vide, ce test n'exercait plus rien
  // du bitmap - il passait par le chemin FIFO ordinaire.
  assert(fillRingWithPumpTargets(im, 1, COMMAND_QUEUE_SIZE) == COMMAND_QUEUE_SIZE);

  // Plus de relachements que la borne. Chaque noteOff() enfile un evenement
  // FORCE : au-dela de EVENT_QUEUE_SIZE evenements d'un coup, la rafale se
  // mangerait elle-meme en evincant ses propres membres.
  const uint8_t burst = (uint8_t)(INSTRUMENT_MAX_NOTE_OFFS_PER_UPDATE + 4);
  for (uint8_t i = 0; i < burst; i++) assert(im->postCommand(ACMD_NOTE_OFF, (uint8_t)(40 + i)));
  assert(im->commandQueue().hasPendingNoteOff());

  // UNE passe n'ecoule pas toute la rafale : il en reste.
  __test_millis += 10;
  im->update();
  assert(im->commandQueue().hasPendingNoteOff());

  // Mais tout finit par passer, en un nombre de passes fini.
  int passes = 1;
  while (im->commandQueue().hasPendingNoteOff() && passes < 32) {
    __test_millis += 10;
    im->update();
    passes++;
  }
  assert(!im->commandQueue().hasPendingNoteOff());
  // L'anneau est plein, donc la garde de report entre desormais en jeu ici
  // aussi : c'est la meme borne que celle du test frere, pas une borne relachee.
  // L'ancienne formule ne comptait pas les passes cedees a l'anneau.
  assert(passes <= (int)(INSTRUMENT_MAX_NOTE_OFF_DEFERRAL_PASSES + 1) *
                       (int)((burst + INSTRUMENT_MAX_NOTE_OFFS_PER_UPDATE - 1) /
                             INSTRUMENT_MAX_NOTE_OFFS_PER_UPDATE));

  delete im;
}

void every_pending_note_off_is_applied_even_under_permanent_saturation() {
  tasksResetCfg();
  InstrumentManager* im = makeReadyInstrument();
  __test_millis = 1000;

  // Plusieurs relachements en attente, et un client qui REMPLIT l'anneau a
  // chaque passe : l'anneau n'est donc jamais vide. Sans garde de report, les
  // relachements n'arriveraient jamais.
  const uint8_t notes[] = {60, 61, 62, 64, 67};
  // Meme raison qu'au test precedent : l'anneau doit etre plein pour que les
  // relachements atterrissent dans le bitmap plutot que dans la file.
  assert(fillRingWithPumpTargets(im, 1, COMMAND_QUEUE_SIZE) == COMMAND_QUEUE_SIZE);
  for (uint8_t n : notes) assert(im->postCommand(ACMD_NOTE_OFF, n));
  assert(im->commandQueue().hasPendingNoteOff());

  int passes = 0;
  while (im->commandQueue().hasPendingNoteOff() && passes < 64) {
    fillRingWithPumpTargets(im, 1, COMMAND_QUEUE_SIZE);   // saturation permanente
    __test_millis += 10;
    im->update();
    passes++;
  }
  assert(!im->commandQueue().hasPendingNoteOff());
  // Borne de report + borne de relachements par passe : le pire cas reste petit
  // et connu, jamais "quand l'anneau voudra bien se vider".
  const int worstCase =
      (int)(INSTRUMENT_MAX_NOTE_OFF_DEFERRAL_PASSES + 1) *
      (int)((sizeof(notes) + INSTRUMENT_MAX_NOTE_OFFS_PER_UPDATE - 1) /
            INSTRUMENT_MAX_NOTE_OFFS_PER_UPDATE);
  assert(passes <= worstCase);

  delete im;
}

/*=============================================================================
 * C-3 - une allocation refusee ne doit rien faire planter
 *
 * `new T[n]` sans std::nothrow leve sur un tas fragmente : sans gestionnaire,
 * la carte redemarre. Et si l'allocation echoue, tout acces ulterieur
 * dereference un pointeur nul.
 *
 * INJECTION : capacite nulle. Le constructeur fait converger les deux cas (new
 * refuse, capacite nulle) vers le MEME etat interne - tableau nullptr, capacite
 * 0 - donc ce que ce test exerce est exactement le chemin degrade d'une
 * allocation refusee, sans avoir a fragmenter le tas de l'hote.
 *===========================================================================*/

void a_queue_without_storage_refuses_instead_of_crashing() {
  // --- CommandQueue ----------------------------------------------------------
  CommandQueue q(0);
  assert(!q.storageAvailable());
  assert(q.count() == 0);

  ActuatorCommand out;
  assert(!q.pop(out));                              // pas de deref
  assert(!q.push(ActuatorCommand(ACMD_PUMP_TARGET, 0, 50)));
  assert(q.count() == 0);
  assert(q.droppedCount() == 1);                    // la panne est OBSERVABLE
  assert(!q.pop(out));

  // LE POINT PRECIEUX : le panic et les Note Off ne vivent pas dans le tableau
  // alloue (drapeau dedie + bitmap statique). Ils doivent donc survivre a la
  // panne d'allocation - c'est ce qui reste de securite quand le tas est mort.
  q.requestPanic();
  assert(q.panicPending());
  assert(q.takePanicRequest());
  assert(!q.takePanicRequest());

  q.requestNoteOff(61);
  q.requestNoteOff(60);
  assert(q.hasPendingNoteOff());
  uint8_t note = 255;
  assert(q.takePendingNoteOff(note) && note == 60);
  assert(q.takePendingNoteOff(note) && note == 61);
  assert(!q.takePendingNoteOff(note));
  assert(!q.hasPendingNoteOff());

  q.clear();                                        // ne dereference rien
  q.resetDroppedCount();
  assert(q.droppedCount() == 0);
  assert(q.count() == 0);

  // --- EventQueue ------------------------------------------------------------
  EventQueue eq(0);
  assert(!eq.storageAvailable());
  assert(eq.getCount() == 0);
  assert(eq.isEmpty());
  assert(eq.isFull());                              // pleine = refuse tout
  assert(!eq.enqueueLiveEvent(EVENT_NOTE_ON, 60, 100));
  assert(!eq.enqueueScheduledEvent(EVENT_NOTE_ON, 60, 100, 1000));
  // La variante FORCEE evince normalement le plus ancien pour reussir toujours :
  // sans stockage, elle doit repondre false au lieu d'ecrire dans le vide.
  assert(!eq.enqueueLiveEventForced(EVENT_NOTE_OFF, 60, 0));
  assert(!eq.enqueueScheduledEventForced(EVENT_NOTE_OFF, 60, 0, 1000));
  assert(eq.getCount() == 0);

  MidiEvent ev;
  assert(!eq.peekCopy(ev));
  assert(!eq.tryPopDueEvent(millis(), 0, ev));
  eq.dequeue();                                     // ne dereference rien
  assert(eq.getReferenceTime() == 0);
  const uint32_t epochBefore = eq.epoch();
  eq.clear();
  assert(eq.epoch() == epochBefore + 1);
  assert(eq.getCount() == 0);
}

}  // namespace

void harden_tasks_run_all_tests() {
  reset_controllers_request_is_taken_and_cleared_in_one_go();
  power_on_request_is_taken_and_cleared_in_one_go();
  command_work_is_bounded_per_update_and_nothing_is_lost();
  panic_is_executed_in_the_saturated_pass();
  pending_note_offs_are_never_lost_and_keep_the_ring_first_order();
  a_burst_of_note_offs_is_bounded_per_pass_and_fully_applied();
  every_pending_note_off_is_applied_even_under_permanent_saturation();
  a_queue_without_storage_refuses_instead_of_crashing();
}

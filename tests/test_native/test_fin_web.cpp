/***********************************************************************************************
 * test_fin_web - LOT C : la couche web
 *
 * POURQUOI CES TESTS PORTENT SUR DES MODULES EXTRAITS
 * ---------------------------------------------------
 * `WebConfigurator.cpp` n'est compilable sur AUCUN build hote : il inclut
 * ESPAsyncWebServer / AsyncTCP, qui n'existent pas ici. Un test ecrit contre
 * lui ne serait donc qu'une lecture de son texte source, et une lecture ne
 * demontre rien.
 *
 * C'est la raison d'etre de l'extraction : la logique dont depend la surete -
 * le hand-off AsyncTCP <-> loop(), la prise indivisible d'une demande, la
 * lecture coherente de la configuration - vit maintenant dans
 * `WebOpChannel.{h,cpp}` et `ConfigSnapshot.{h,cpp}`, purs, sans FreeRTOS et
 * sans Arduino au-dela de `ConfigStorage.h`, avec leurs primitives INJECTEES.
 * Ces deux modules-la sont compiles et EXECUTES ici.
 *
 * CE QUI EST PROUVE, ET CE QUI NE L'EST PAS
 * -----------------------------------------
 * Un test hote est MONO-TACHE et les sections critiques du stub sont des
 * no-ops. On prouve donc des CONTRATS - "une operation publiee est executee
 * exactement une fois", "un appelant recoit SON resultat", "un verrou refuse ne
 * laisse aucune copie partielle", "une demande n'est jamais perdue" - et le
 * COMPTAGE exact des prises et relachements de verrou. On ne prouve PAS
 * l'absence de course : aucun ordonnancement reel n'est explore ici. Les
 * entrelacements qui comptent sont donc joues a la main, en appelant les deux
 * cotes de la machine dans l'ordre voulu.
 *
 * Ce qui reste dans le fichier non compilable (C-1, C-5, C-6) est couvert par
 * des GARDES DE SOURCE en fin de fichier. Ce sont des gardes, PAS des preuves
 * comportementales : ils verifient une structure, pas un comportement.
 *
 * Point d'entree : fin_web_run_all_tests().
 ***********************************************************************************************/
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "ConfigSnapshot.h"
#include "ConfigStorage.h"
#include "WebOpChannel.h"

namespace {

/* ===========================================================================
 * Faux systeme de primitives pour WebOpChannel
 *
 * Il COMPTE tout : prises et relachements du verrou de producteur, entrees et
 * sorties de section critique, signaux poses et consommes. C'est ce comptage
 * qui permet d'affirmer "aucun chemin ne relache un verrou qu'il n'a pas pris,
 * ni ne le prend deux fois" - une relecture ne le permettrait pas.
 *
 * Le temps est SIMULE : `now` n'avance que lorsqu'on le demande, ou d'une
 * milliseconde a chaque attente. Un test qui depend de millis() reel serait
 * lent et capricieux.
 * ========================================================================= */
struct ChanHarness {
  // Comptage des verrous
  int producerLocks = 0;
  int producerUnlocks = 0;
  int producerHeld = 0;         // doit rester dans {0, 1} a tout instant
  int maxProducerHeld = 0;
  bool producerReentered = false;
  bool producerUnderflow = false;

  int stateEnters = 0;
  int stateExits = 0;
  int stateDepth = 0;
  int maxStateDepth = 0;

  // Semaphore binaire simule
  int signals = 0;              // poses
  int drains = 0;
  bool doneFlag = false;

  // Temps simule
  uint32_t now = 1000;
  int yields = 0;

  // Refus a volonte
  bool refuseProducerLock = false;
};

bool hLockProducer(void* ctx, uint32_t timeoutMs) {
  ChanHarness* h = static_cast<ChanHarness*>(ctx);
  (void)timeoutMs;
  if (h->refuseProducerLock) return false;
  if (h->producerHeld > 0) h->producerReentered = true;   // prise double
  h->producerHeld++;
  if (h->producerHeld > h->maxProducerHeld) h->maxProducerHeld = h->producerHeld;
  h->producerLocks++;
  return true;
}

void hUnlockProducer(void* ctx) {
  ChanHarness* h = static_cast<ChanHarness*>(ctx);
  if (h->producerHeld <= 0) h->producerUnderflow = true;  // relache sans prise
  h->producerHeld--;
  h->producerUnlocks++;
}

void hEnterState(void* ctx) {
  ChanHarness* h = static_cast<ChanHarness*>(ctx);
  h->stateEnters++;
  h->stateDepth++;
  if (h->stateDepth > h->maxStateDepth) h->maxStateDepth = h->stateDepth;
}

void hExitState(void* ctx) {
  ChanHarness* h = static_cast<ChanHarness*>(ctx);
  h->stateDepth--;
  h->stateExits++;
}

bool hWaitDone(void* ctx, uint32_t timeoutMs) {
  ChanHarness* h = static_cast<ChanHarness*>(ctx);
  if (h->doneFlag) {
    h->doneFlag = false;
    return true;
  }
  // Rien a attendre dans un test mono-tache : on laisse le temps avancer
  // jusqu'a l'echeance, ce qui reproduit exactement le cas "loop() ne repond
  // pas".
  h->now += timeoutMs;
  return false;
}

void hSignalDone(void* ctx) {
  ChanHarness* h = static_cast<ChanHarness*>(ctx);
  h->doneFlag = true;
  h->signals++;
}

void hDrainDone(void* ctx) {
  ChanHarness* h = static_cast<ChanHarness*>(ctx);
  h->drains++;
  h->doneFlag = false;
}

uint32_t hNow(void* ctx) { return static_cast<ChanHarness*>(ctx)->now; }

void hYield(void* ctx, uint32_t ms) {
  ChanHarness* h = static_cast<ChanHarness*>(ctx);
  h->yields++;
  h->now += ms;
}

WebOpChannelOps chanOpsFor(ChanHarness& h) {
  WebOpChannelOps ops;
  ops.lockProducer = hLockProducer;
  ops.unlockProducer = hUnlockProducer;
  ops.enterState = hEnterState;
  ops.exitState = hExitState;
  ops.waitDone = hWaitDone;
  ops.signalDone = hSignalDone;
  ops.drainDone = hDrainDone;
  ops.nowMs = hNow;
  ops.yieldMs = hYield;
  ops.ctx = &h;
  return ops;
}

// Charge utile de test : ce que WebConfigurator garde dans son `_op`. Elle est
// VOLONTAIREMENT hors du canal - c'est le contrat : le canal dit qui possede
// l'emplacement, il ne le transporte pas.
struct FakeOp {
  uint32_t seq;
  int input;
  int result;
  bool applied;
  FakeOp() : seq(0), input(0), result(0), applied(false) {}
};

// Le cote loop() : exactement ce que fait servicePendingOp().
// Rend true si une operation a ete prise, et renseigne `executed`.
bool serviceOnce(WebOpChannel& chan, FakeOp& slot, int& executions, bool& executed) {
  WebOpClaim taken;
  executed = false;
  if (!chan.claim(taken)) return false;
  if (taken.execute) {
    executions++;
    executed = true;
    slot.result = slot.input * 10;
    slot.applied = true;
  }
  chan.complete(taken);
  return true;
}

/* ===========================================================================
 * C-4 - la machine de hand-off
 * ========================================================================= */

// Le cas nominal : publier, faire tourner loop(), recuperer SON resultat.
void web_op_nominal_round_trip() {
  ChanHarness h;
  WebOpChannel chan;
  chan.begin(chanOpsFor(h), 50);
  FakeOp slot;
  int executions = 0;
  bool executed = false;

  WebOpTicket t;
  assert(chan.beginPublish(t));
  assert(t.seq != 0);
  assert(chan.state() == WEBOP_SLOT_IDLE);   // pas encore publie

  slot.seq = t.seq;
  slot.input = 7;
  chan.publish(t);
  assert(chan.state() == WEBOP_SLOT_ARMED);
  assert(chan.pending());

  // loop() passe.
  assert(serviceOnce(chan, slot, executions, executed));
  assert(executed);
  assert(executions == 1);
  assert(chan.state() == WEBOP_SLOT_DONE);

  assert(chan.awaitResult(t));
  assert(t.collected);
  assert(slot.result == 70);
  assert(chan.lastDoneSeq() == t.seq);

  chan.endPublish(t);
  // L'emplacement est rendu : un autre producteur peut s'en servir.
  assert(chan.state() == WEBOP_SLOT_IDLE);
  assert(!chan.pending());
  assert(h.producerHeld == 0);
}

// Une operation publiee est executee EXACTEMENT une fois, meme si loop()
// repasse. C'est la transition ARMED -> RUNNING, indivisible, qui le garantit.
void web_op_is_executed_exactly_once() {
  ChanHarness h;
  WebOpChannel chan;
  chan.begin(chanOpsFor(h), 50);
  FakeOp slot;
  int executions = 0;
  bool executed = false;

  WebOpTicket t;
  assert(chan.beginPublish(t));
  slot.input = 3;
  chan.publish(t);

  assert(serviceOnce(chan, slot, executions, executed));
  assert(executions == 1);
  // loop() repasse cinq fois : il n'y a plus rien a prendre.
  for (int i = 0; i < 5; i++) {
    assert(!serviceOnce(chan, slot, executions, executed));
  }
  assert(executions == 1);

  assert(chan.awaitResult(t));
  chan.endPublish(t);
  assert(executions == 1);
}

// Deux operations successives portent des numeros de sequence DIFFERENTS, et
// chaque appelant recoit le sien. Sans cela, un appelant pouvait prendre le
// resultat d'une operation precedente pour le sien.
void web_op_sequence_distinguishes_successive_ops() {
  ChanHarness h;
  WebOpChannel chan;
  chan.begin(chanOpsFor(h), 50);
  FakeOp slot;
  int executions = 0;
  bool executed = false;

  WebOpTicket first;
  assert(chan.beginPublish(first));
  slot.input = 2;
  chan.publish(first);
  assert(serviceOnce(chan, slot, executions, executed));
  assert(chan.awaitResult(first));
  const int firstResult = slot.result;
  chan.endPublish(first);

  WebOpTicket second;
  assert(chan.beginPublish(second));
  assert(second.seq != first.seq);
  slot.input = 5;
  chan.publish(second);
  assert(serviceOnce(chan, slot, executions, executed));
  assert(chan.awaitResult(second));
  chan.endPublish(second);

  assert(firstResult == 20);
  assert(slot.result == 50);
  assert(chan.lastDoneSeq() == second.seq);
  assert(executions == 2);
}

// L'APPELANT RECOIT SON RESULTAT, PAS CELUI D'AVANT. On force le cas : le
// signal de fin d'une operation abandonnee traine encore quand la suivante est
// publiee. Sans le drainage + la verification de sequence, l'attente de la
// seconde reviendrait immediatement sur le resultat de la premiere.
void web_op_stale_completion_is_not_taken_for_a_fresh_one() {
  ChanHarness h;
  WebOpChannel chan;
  chan.begin(chanOpsFor(h), 50);
  FakeOp slot;
  int executions = 0;
  bool executed = false;

  // Premiere operation : publiee, abandonnee, PUIS executee par loop().
  WebOpTicket first;
  assert(chan.beginPublish(first));
  slot.input = 1;
  chan.publish(first);
  assert(!chan.awaitResult(first));      // echeance : abandon
  assert(chan.abandoned());
  chan.endPublish(first);

  // loop() la prend : elle est abandonnee, donc PAS appliquee - et surtout
  // aucun signal n'est laisse derriere elle.
  assert(serviceOnce(chan, slot, executions, executed));
  assert(!executed);
  assert(executions == 0);
  assert(h.signals == 0);
  assert(chan.state() == WEBOP_SLOT_IDLE);

  // Seconde operation : elle doit attendre SON resultat.
  WebOpTicket second;
  assert(chan.beginPublish(second));
  assert(second.seq != first.seq);
  slot.input = 4;
  chan.publish(second);
  assert(serviceOnce(chan, slot, executions, executed));
  assert(executed);
  assert(chan.awaitResult(second));
  assert(slot.result == 40);
  assert(chan.lastDoneSeq() == second.seq);
  chan.endPublish(second);
}

// UN APPELANT QUI RENONCE NE FAIT PAS APPLIQUER SON OPERATION APRES COUP.
// C'est le defaut de fond : l'emplacement portait encore une operation dont
// plus personne n'attendait le resultat, et rien n'empechait loop() de
// l'executer - avec ses effets de bord (commit de configuration, ecriture
// LittleFS, redemarrage programme) - bien apres que la requete HTTP a rendu
// une erreur.
void web_op_abandoned_caller_does_not_apply_later() {
  ChanHarness h;
  WebOpChannel chan;
  chan.begin(chanOpsFor(h), 50);
  FakeOp slot;
  int executions = 0;
  bool executed = false;

  WebOpTicket t;
  assert(chan.beginPublish(t));
  slot.input = 9;
  chan.publish(t);
  assert(!chan.awaitResult(t));
  chan.endPublish(t);

  // loop() finit par passer : l'operation est prise pour etre LIBEREE, pas
  // appliquee.
  assert(serviceOnce(chan, slot, executions, executed));
  assert(!executed);
  assert(executions == 0);
  assert(!slot.applied);
  assert(slot.result == 0);
  assert(chan.state() == WEBOP_SLOT_IDLE);
}

// ... ET IL N'ECRASE PAS LE RESULTAT D'UN AUTRE. Un producteur qui a renonce ne
// peut pas liberer l'emplacement tant que loop() ne l'a pas rendu : sinon le
// producteur suivant ecrirait sa charge utile dans un emplacement que loop()
// est en train de lire.
void web_op_abandoned_caller_cannot_steal_the_slot() {
  ChanHarness h;
  WebOpChannel chan;
  chan.begin(chanOpsFor(h), 50);
  FakeOp slot;
  int executions = 0;
  bool executed = false;

  WebOpTicket first;
  assert(chan.beginPublish(first));
  slot.input = 6;
  chan.publish(first);
  assert(!chan.awaitResult(first));     // renonce
  chan.endPublish(first);

  // L'emplacement est encore ARMED : loop() n'y a pas touche.
  assert(chan.state() == WEBOP_SLOT_ARMED);

  // Un second producteur se presente AVANT que loop() n'ait libere. Il doit
  // etre refuse plutot que d'ecraser.
  WebOpTicket second;
  const uint32_t before = h.now;
  assert(!chan.beginPublish(second));
  assert(second.seq == 0);
  assert(!second.holdsProducerLock);
  assert(h.now > before);               // il a bien attendu, puis renonce
  assert(h.producerHeld == 0);          // et il a rendu le verrou
  assert(h.producerLocks == h.producerUnlocks);

  // loop() libere enfin.
  assert(serviceOnce(chan, slot, executions, executed));
  assert(!executed);
  assert(chan.state() == WEBOP_SLOT_IDLE);

  // Le second producteur peut maintenant passer, et il obtient SON resultat.
  WebOpTicket third;
  assert(chan.beginPublish(third));
  assert(third.seq != first.seq);
  slot.input = 8;
  chan.publish(third);
  assert(serviceOnce(chan, slot, executions, executed));
  assert(executed);
  assert(chan.awaitResult(third));
  assert(slot.result == 80);
  chan.endPublish(third);
}

// LE CAS LIMITE QUI FAISAIT FUIR L'EMPLACEMENT : loop() publie le resultat au
// moment meme ou l'appelant atteint son echeance. L'abandon et la publication
// sont decides dans LA MEME section critique : l'appelant voit alors le
// resultat et le COLLECTE, au lieu de declarer l'operation abandonnee et de
// laisser l'emplacement occupe pour toujours.
void web_op_result_published_at_the_deadline_is_still_collected() {
  ChanHarness h;
  WebOpChannel chan;
  chan.begin(chanOpsFor(h), 50);
  FakeOp slot;
  int executions = 0;
  bool executed = false;

  WebOpTicket t;
  assert(chan.beginPublish(t));
  slot.input = 11;
  chan.publish(t);

  // loop() execute AVANT que l'appelant n'attende, puis l'appelant arrive en
  // retard : le resultat est la, il doit etre pris et non jete.
  assert(serviceOnce(chan, slot, executions, executed));
  assert(executed);
  h.now += 10000;                      // largement au-dela de l'echeance
  assert(chan.awaitResult(t));         // le resultat prime sur l'echeance
  assert(!chan.abandoned());
  assert(slot.result == 110);
  chan.endPublish(t);
  assert(chan.state() == WEBOP_SLOT_IDLE);
}

// AUCUN CHEMIN NE RELACHE UN VERROU QU'IL N'A PAS PRIS, NI NE LE PREND DEUX
// FOIS. On compte, sur les quatre issues possibles.
void web_op_lock_discipline_is_balanced() {
  // 1. Aller-retour complet.
  {
    ChanHarness h;
    WebOpChannel chan;
    chan.begin(chanOpsFor(h), 50);
    FakeOp slot;
    int executions = 0;
    bool executed = false;
    WebOpTicket t;
    assert(chan.beginPublish(t));
    chan.publish(t);
    serviceOnce(chan, slot, executions, executed);
    assert(chan.awaitResult(t));
    chan.endPublish(t);
    assert(h.producerLocks == 1 && h.producerUnlocks == 1);
    assert(!h.producerReentered && !h.producerUnderflow);
    assert(h.maxProducerHeld == 1 && h.producerHeld == 0);
    assert(h.stateEnters == h.stateExits && h.maxStateDepth == 1);
  }
  // 2. Abandon : le verrou est quand meme rendu, une seule fois.
  {
    ChanHarness h;
    WebOpChannel chan;
    chan.begin(chanOpsFor(h), 50);
    WebOpTicket t;
    assert(chan.beginPublish(t));
    chan.publish(t);
    assert(!chan.awaitResult(t));
    chan.endPublish(t);
    assert(h.producerLocks == 1 && h.producerUnlocks == 1);
    assert(h.producerHeld == 0 && !h.producerUnderflow);
    assert(h.stateEnters == h.stateExits);
  }
  // 3. endPublish() rappelee sur un jeton deja rendu : elle ne relache RIEN.
  {
    ChanHarness h;
    WebOpChannel chan;
    chan.begin(chanOpsFor(h), 50);
    FakeOp slot;
    int executions = 0;
    bool executed = false;
    WebOpTicket t;
    assert(chan.beginPublish(t));
    chan.publish(t);
    serviceOnce(chan, slot, executions, executed);
    assert(chan.awaitResult(t));
    chan.endPublish(t);
    chan.endPublish(t);
    chan.endPublish(t);
    assert(h.producerUnlocks == 1);
    assert(!h.producerUnderflow);
  }
  // 4. Le verrou de producteur est REFUSE : rien n'est pris, donc rien n'est
  //    rendu, et l'appelant garde ce qu'il portait.
  {
    ChanHarness h;
    h.refuseProducerLock = true;
    WebOpChannel chan;
    chan.begin(chanOpsFor(h), 50);
    WebOpTicket t;
    assert(!chan.beginPublish(t));
    assert(!t.holdsProducerLock);
    chan.endPublish(t);
    assert(h.producerLocks == 0 && h.producerUnlocks == 0);
    assert(!h.producerUnderflow);
    // Un jeton non arme ne publie rien et n'attend rien.
    chan.publish(t);
    assert(chan.state() == WEBOP_SLOT_IDLE);
    assert(!chan.awaitResult(t));
  }
  // 5. Emplacement occupe : beginPublish() rend le verrou qu'elle a pris.
  {
    ChanHarness h;
    WebOpChannel chan;
    chan.begin(chanOpsFor(h), 20);
    WebOpTicket first;
    assert(chan.beginPublish(first));
    chan.publish(first);
    assert(!chan.awaitResult(first));
    chan.endPublish(first);
    assert(h.producerLocks == 1 && h.producerUnlocks == 1);
    WebOpTicket second;
    assert(!chan.beginPublish(second));
    assert(h.producerLocks == 2 && h.producerUnlocks == 2);
    assert(h.producerHeld == 0 && !h.producerReentered && !h.producerUnderflow);
  }
}

// Primitives absentes (tas epuise au demarrage : pas de mutex, pas de
// semaphore) : le canal echoue en FERMETURE. Il ne pretend jamais avoir
// serialise ce qu'il n'a pas serialise.
void web_op_unusable_channel_fails_closed() {
  WebOpChannel chan;   // jamais begin()
  WebOpTicket t;
  assert(!chan.beginPublish(t));
  chan.publish(t);
  assert(!chan.awaitResult(t));
  chan.endPublish(t);
  WebOpClaim c;
  assert(!chan.claim(c));
  chan.complete(c);
  assert(chan.state() == WEBOP_SLOT_IDLE);
  assert(!chan.pending());
  assert(chan.lastDoneSeq() == 0);
  assert(!chan.abandoned());

  // Un jeu de primitives INCOMPLET est refuse comme un jeu absent : accepter
  // le lot en laissant un pointeur nul ferait planter au premier appel.
  ChanHarness h;
  WebOpChannelOps ops = chanOpsFor(h);
  ops.signalDone = nullptr;
  WebOpChannel partial;
  partial.begin(ops, 50);
  WebOpTicket t2;
  assert(!partial.beginPublish(t2));
  assert(h.producerLocks == 0);
}

/* ===========================================================================
 * C-3 - la demande non perdable (annulation de calibration)
 * ========================================================================= */

struct LatchHarness {
  int enters = 0;
  int exits = 0;
  int depth = 0;
  int maxDepth = 0;
};

void lEnter(void* ctx) {
  LatchHarness* h = static_cast<LatchHarness*>(ctx);
  h->enters++;
  h->depth++;
  if (h->depth > h->maxDepth) h->maxDepth = h->depth;
}
void lExit(void* ctx) {
  LatchHarness* h = static_cast<LatchHarness*>(ctx);
  h->depth--;
  h->exits++;
}

void latched_request_is_never_lost_and_coalesces() {
  LatchHarness h;
  LatchedRequest req;
  req.begin(lEnter, lExit, &h);

  // Rien de pose : rien a prendre.
  assert(!req.pending());
  assert(!req.take());

  // Pose, prise -> true ; seconde prise -> false.
  req.request();
  assert(req.pending());
  assert(req.take());
  assert(!req.take());
  assert(!req.pending());

  // DEUX poses -> UNE seule prise vraie. Coalescence : on annule une fois, pas
  // deux. Jamais de perte, jamais de doublon.
  req.request();
  req.request();
  assert(req.take());
  assert(!req.take());

  // Les sections critiques sont equilibrees et jamais imbriquees.
  assert(h.enters == h.exits);
  assert(h.maxDepth == 1);
  assert(h.depth == 0);
}

// LE CAS QUI PERDAIT LA DEMANDE. L'ancien motif etait :
//     if (_calCancelRequested) { _calCancelRequested = false; cancel(); }
// Une demande deposee ENTRE le test et l'effacement etait effacee sans avoir
// ete traitee. Ici, la prise est faite AVANT le traitement, d'un seul tenant :
// une demande deposee pendant le traitement survit.
void latched_request_posted_during_handling_survives() {
  LatchHarness h;
  LatchedRequest req;
  req.begin(lEnter, lExit, &h);

  req.request();
  const bool firstTake = req.take();       // prise AVANT le traitement
  assert(firstTake);
  // "traitement" de la premiere demande ; une seconde arrive pendant.
  req.request();
  // Elle n'a pas ete effacee par le traitement en cours.
  assert(req.pending());
  assert(req.take());
  assert(!req.take());
}

// Sans begin(), l'objet echoue en fermeture : il ne manipule pas un drapeau
// que rien ne protege.
void latched_request_without_primitives_fails_closed() {
  LatchedRequest req;
  req.request();
  assert(!req.pending());
  assert(!req.take());
}

/* ===========================================================================
 * C-2 - la copie coherente de la configuration
 * ========================================================================= */

struct LockHarness {
  int locks = 0;
  int unlocks = 0;
  int held = 0;
  int maxHeld = 0;
  bool refuse = false;
  bool unlockedWithoutLock = false;
};

bool cLock(void* ctx) {
  LockHarness* h = static_cast<LockHarness*>(ctx);
  if (h->refuse) return false;
  h->locks++;
  h->held++;
  if (h->held > h->maxHeld) h->maxHeld = h->held;
  return true;
}

void cUnlock(void* ctx) {
  LockHarness* h = static_cast<LockHarness*>(ctx);
  if (h->held <= 0) h->unlockedWithoutLock = true;
  h->held--;
  h->unlocks++;
}

ConfigLockOps lockOpsFor(LockHarness& h) {
  ConfigLockOps ops;
  ops.lock = cLock;
  ops.unlock = cUnlock;
  ops.ctx = &h;
  return ops;
}

// Remplit une RuntimeConfig d'un motif reconnaissable, octets de bourrage
// compris : c'est ce qui rend le memcmp deterministe.
void fillConfigPattern(RuntimeConfig& c, unsigned char seed) {
  unsigned char* raw = reinterpret_cast<unsigned char*>(&c);
  for (size_t i = 0; i < sizeof(RuntimeConfig); i++) {
    raw[i] = (unsigned char)((i * 31u + seed) & 0xFF);
  }
}

void snapshot_under_granted_lock_is_bit_exact() {
  LockHarness h;
  RuntimeConfig src;
  RuntimeConfig dst;
  fillConfigPattern(src, 7);
  fillConfigPattern(dst, 200);
  assert(memcmp(&src, &dst, sizeof(RuntimeConfig)) != 0);

  assert(snapshotConfig(lockOpsFor(h), src, dst));
  // Copie BIT A BIT : pas "les champs qu'on a pense a recopier".
  assert(memcmp(&src, &dst, sizeof(RuntimeConfig)) == 0);
  // Le verrou est pris une fois et relache EXACTEMENT une fois.
  assert(h.locks == 1 && h.unlocks == 1);
  assert(h.held == 0 && h.maxHeld == 1);
  assert(!h.unlockedWithoutLock);
}

// LE DEFAUT CORRIGE : la copie etait faite SANS verrou. Ici on exige qu'un
// verrou refuse produise un refus, et surtout qu'il ne laisse AUCUNE copie
// partielle - c'est le seul etat dont l'appelant ne saurait rien faire.
void snapshot_under_refused_lock_copies_nothing() {
  LockHarness h;
  h.refuse = true;
  RuntimeConfig src;
  RuntimeConfig dst;
  RuntimeConfig witness;
  fillConfigPattern(src, 7);
  fillConfigPattern(dst, 200);
  memcpy(&witness, &dst, sizeof(RuntimeConfig));

  assert(!snapshotConfig(lockOpsFor(h), src, dst));
  // Destination INTACTE, jusqu'au dernier octet.
  assert(memcmp(&witness, &dst, sizeof(RuntimeConfig)) == 0);
  // Et rien n'a ete relache, puisque rien n'a ete pris.
  assert(h.locks == 0 && h.unlocks == 0);
  assert(!h.unlockedWithoutLock);
}

void snapshot_rejects_unusable_ops() {
  RuntimeConfig src;
  RuntimeConfig dst;
  RuntimeConfig witness;
  fillConfigPattern(src, 3);
  fillConfigPattern(dst, 99);
  memcpy(&witness, &dst, sizeof(RuntimeConfig));

  ConfigLockOps broken;
  broken.lock = nullptr;
  broken.unlock = cUnlock;
  broken.ctx = nullptr;
  assert(!snapshotConfig(broken, src, dst));
  assert(memcmp(&witness, &dst, sizeof(RuntimeConfig)) == 0);

  LockHarness h;
  ConfigLockOps half = lockOpsFor(h);
  half.unlock = nullptr;
  assert(!snapshotConfig(half, src, dst));
  assert(memcmp(&witness, &dst, sizeof(RuntimeConfig)) == 0);
  assert(h.locks == 0);
}

// La chaine copiee est TOUJOURS terminee, meme quand la source ne l'est pas -
// c'est precisement l'etat qu'une copie a moitie faite peut produire, et le
// serialiseur JSON lirait alors au-dela du tableau.
void snapshot_string_is_always_terminated() {
  LockHarness h;
  char src[8];
  memset(src, 'A', sizeof(src));      // AUCUN '\0' : la source est "dechiree"
  char dst[8];
  memset(dst, 'Z', sizeof(dst));

  assert(snapshotConfigString(lockOpsFor(h), src, dst, sizeof(dst)));
  assert(strlen(dst) == sizeof(dst) - 1);
  assert(dst[sizeof(dst) - 1] == '\0');
  assert(h.locks == 1 && h.unlocks == 1 && h.held == 0);

  // Source plus courte : copie exacte.
  LockHarness h2;
  const char* shortSrc = "wifi";
  char dst2[8];
  memset(dst2, 'Z', sizeof(dst2));
  assert(snapshotConfigString(lockOpsFor(h2), shortSrc, dst2, sizeof(dst2)));
  assert(strcmp(dst2, "wifi") == 0);

  // Verrou refuse : destination intacte, rien de pris.
  LockHarness h3;
  h3.refuse = true;
  char dst3[8];
  memset(dst3, 'Z', sizeof(dst3));
  assert(!snapshotConfigString(lockOpsFor(h3), shortSrc, dst3, sizeof(dst3)));
  for (size_t i = 0; i < sizeof(dst3); i++) assert(dst3[i] == 'Z');
  assert(h3.locks == 0 && h3.unlocks == 0);

  // Pas de place pour le terminateur : refus franc, sans prise de verrou.
  LockHarness h4;
  char dst4[1];
  dst4[0] = 'Z';
  assert(!snapshotConfigString(lockOpsFor(h4), shortSrc, dst4, 0));
  assert(dst4[0] == 'Z');
  assert(h4.locks == 0);
}

// Une copie de zero octet, ou une source confondue avec la destination, ne
// doivent pas faire attendre un commit pour rien.
void snapshot_degenerate_cases_do_not_lock() {
  LockHarness h;
  RuntimeConfig c;
  fillConfigPattern(c, 5);
  assert(snapshotConfigBytes(lockOpsFor(h), &c, &c, sizeof(RuntimeConfig)));
  assert(h.locks == 0);
  assert(snapshotConfigBytes(lockOpsFor(h), &c, &c, 0));
  assert(h.locks == 0);

  RuntimeConfig other;
  fillConfigPattern(other, 6);
  RuntimeConfig witness;
  memcpy(&witness, &other, sizeof(RuntimeConfig));
  assert(snapshotConfigBytes(lockOpsFor(h), &c, &other, 0));
  assert(memcmp(&witness, &other, sizeof(RuntimeConfig)) == 0);
  assert(h.locks == 0);

  // Pointeur nul : refus, sans verrou.
  assert(!snapshotConfigBytes(lockOpsFor(h), nullptr, &other, 4));
  assert(!snapshotConfigBytes(lockOpsFor(h), &c, nullptr, 4));
  assert(h.locks == 0);
}

/* ===========================================================================
 * GARDES DE SOURCE - C-1, C-5, C-6
 *
 * CE NE SONT PAS DES PREUVES COMPORTEMENTALES. `WebConfigurator.cpp` inclut
 * ESPAsyncWebServer et AsyncTCP : il ne se compile sur aucun build hote, donc
 * rien de ce qu'il contient ne peut etre EXECUTE ici. Ces gardes verifient une
 * STRUCTURE - un ordre d'instructions, une valeur de retour consommee - pour
 * qu'une regression evidente ne passe pas inapercue entre deux relectures.
 * Ils ne disent rien de ce que le firmware fait reellement sur la carte.
 * ========================================================================= */

std::string finWebRead(const char* relPath) {
  // Les tests tournent depuis la racine du depot (harnais pytest comme
  // PlatformIO). Deux chemins sont essayes pour ne pas dependre du repertoire
  // courant exact.
  static const char* const kPrefixes[] = {"", "../", "../../"};
  for (unsigned i = 0; i < sizeof(kPrefixes) / sizeof(kPrefixes[0]); i++) {
    std::string path = std::string(kPrefixes[i]) + relPath;
    std::ifstream in(path.c_str());
    if (!in.good()) continue;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
  }
  return std::string();
}

// Retire les commentaires : un nom cite en prose n'est pas du code, et le
// compter ferait passer une regression pour une protection.
std::string finWebCodeOnly(const std::string& src) {
  std::string out;
  out.reserve(src.size());
  bool inLine = false, inBlock = false;
  for (size_t i = 0; i < src.size(); i++) {
    if (inLine) {
      if (src[i] == '\n') { inLine = false; out += '\n'; }
      continue;
    }
    if (inBlock) {
      if (src[i] == '*' && i + 1 < src.size() && src[i + 1] == '/') { inBlock = false; i++; }
      continue;
    }
    if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '/') { inLine = true; i++; continue; }
    if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '*') { inBlock = true; i++; continue; }
    out += src[i];
  }
  return out;
}

std::string finWebSection(const std::string& src, const char* from, const char* to) {
  const size_t a = src.find(from);
  if (a == std::string::npos) return std::string();
  const size_t b = src.find(to, a + strlen(from));
  if (b == std::string::npos) return src.substr(a);
  return src.substr(a, b - a);
}

// C-1 (GARDE, pas preuve) : tout `endTestSession(false)` du gestionnaire
// WebSocket est precede, dans la MEME branche, d'une mise en securite dont
// l'acceptation est verifiee - postStopCommand(), qui escalade en panic si
// l'ordre est refuse, ou un requestPanic() explicite.
void guard_c1_every_session_end_is_preceded_by_a_guaranteed_safing() {
  const std::string web = finWebCodeOnly(finWebRead("Servo_flute_ESP32/WebConfigurator.cpp"));
  assert(!web.empty());

  // La primitive existe et VERIFIE la valeur de retour de postCommand().
  const std::string post = finWebSection(web, "bool WebConfigurator::postStopCommand", "\n}\n");
  assert(!post.empty());
  assert(post.find("if (_instrument->postCommand(cmdType, a)) return true;") != std::string::npos);
  assert(post.find("_instrument->requestPanic();") != std::string::npos);

  // pump_stop : l'ordre passe par la primitive, l'index de pompe est transmis,
  // et la session n'est fermee qu'apres.
  const std::string pumpStop = finWebSection(web, "strcmp(type, \"pump_stop\")", "else if");
  assert(!pumpStop.empty());
  assert(pumpStop.find("postStopCommand(ACMD_PUMP_STOP_SINGLE, (uint8_t)pumpIdx)") != std::string::npos);
  assert(pumpStop.find("postStopCommand(ACMD_PUMP_STOP)") != std::string::npos);
  assert(pumpStop.find("_instrument->postCommand(ACMD_PUMP_STOP") == std::string::npos);
  assert(pumpStop.find("postStopCommand") < pumpStop.find("endTestSession(false)"));

  // fan_stop : meme structure.
  const std::string fanStop = finWebSection(web, "strcmp(type, \"fan_stop\")", "\n#if MIC_ENABLED");
  assert(!fanStop.empty());
  assert(fanStop.find("postStopCommand(ACMD_FAN_STOP)") != std::string::npos);
  assert(fanStop.find("_instrument->postCommand(ACMD_FAN_STOP)") == std::string::npos);
  assert(fanStop.find("postStopCommand") < fanStop.find("endTestSession(false)"));

  // panic : la mise en securite est demandee AVANT la fermeture de session.
  const std::string panic = finWebSection(web, "strcmp(type, \"panic\")", "else if");
  assert(!panic.empty());
  assert(panic.find("requestPanic();") < panic.find("endTestSession(false)"));

  // endTestSession() demande le panic AVANT de retirer le filet.
  const std::string endSession = finWebSection(web, "void WebConfigurator::endTestSession", "\n}\n");
  assert(!endSession.empty());
  assert(endSession.find("requestPanic();") < endSession.find("_testActive = false;"));

  // Le seul endTestSession(false) dont la mise en securite est PASSEE et non
  // demandee est celui de l'observation de panique, dans update().
  const std::string upd = finWebSection(web, "void WebConfigurator::update()", "\n}\n");
  assert(upd.find("endTestSession(false);") != std::string::npos);
  assert(upd.find("panicCount()") < upd.find("endTestSession(false);"));
}

// C-5 (GARDE) : les etats de session ne sont plus touches a nu ; ils passent
// par les primitives qui rendent la sequence lire-puis-ecrire indivisible.
void guard_c5_session_state_is_only_touched_through_its_primitives() {
  const std::string web = finWebCodeOnly(finWebRead("Servo_flute_ESP32/WebConfigurator.cpp"));
  const std::string hdrRaw = finWebRead("Servo_flute_ESP32/WebConfigurator.h");
  const std::string hdr = finWebCodeOnly(hdrRaw);
  assert(!web.empty() && !hdr.empty());

  // Le verrou existe et la propriete est DOCUMENTEE dans l'en-tete.
  assert(hdr.find("portMUX_TYPE _sessionMux") != std::string::npos);
  assert(hdrRaw.find("PROPRIETE DES ETATS PARTAGES") != std::string::npos);
  assert(hdr.find("bool testSessionExpired(unsigned long now) const;") != std::string::npos);
  assert(hdr.find("bool takeDueTestNoteOff(unsigned long now, uint8_t& note);") != std::string::npos);

  // `volatile` a disparu des drapeaux inter-taches : il n'est pas une
  // primitive de synchronisation, et le laisser la entretenait l'illusion.
  assert(hdr.find("volatile") == std::string::npos);
  assert(web.find("volatile") == std::string::npos);

  // update() ne lit plus le couple (actif, debut) ni le couple (note,
  // echeance) a nu.
  const std::string upd = finWebSection(web, "void WebConfigurator::update()", "\n}\n");
  assert(!upd.empty());
  assert(upd.find("testSessionExpired(now)") != std::string::npos);
  assert(upd.find("takeDueTestNoteOff(now") != std::string::npos);
  assert(upd.find("_testActive") == std::string::npos);
  assert(upd.find("_testNoteOffTime") == std::string::npos);
  assert(upd.find("_testNoteMidi") == std::string::npos);

  // Les champs ne sont ecrits que dans les quatre primitives dediees. On
  // compte les sections critiques de session : autant d'entrees que de sorties.
  size_t enters = 0, exits = 0, pos = 0;
  while ((pos = web.find("portENTER_CRITICAL(&_sessionMux)", pos)) != std::string::npos) {
    enters++; pos++;
  }
  pos = 0;
  while ((pos = web.find("portEXIT_CRITICAL(&_sessionMux)", pos)) != std::string::npos) {
    exits++; pos++;
  }
  assert(enters == exits);
  assert(enters >= 5);   // begin / expired / owner / arm / take / end

  // La table de sessions WebSocket et le slot d'upload restent a AsyncTCP : la
  // documentation de propriete doit le dire, sans quoi le prochain lecteur
  // refera la course.
  assert(hdrRaw.find("etat de TRANSPORT") != std::string::npos);

  // ... et la propriete du slot d'upload n'est reprise par AsyncTCP que si
  // l'operation a REELLEMENT rendu son resultat. Le liberer apres une echeance
  // viderait des String que loop() est peut-etre en train de lire.
  const std::string uploadDone = finWebSection(
      web, "void WebConfigurator::handleMidiUploadComplete", "\nsize_t WebConfigurator::");
  assert(!uploadDone.empty());
  const size_t runPos = uploadDone.find("runOnLoop(op)");
  assert(runPos != std::string::npos);
  const std::string afterRun = uploadDone.substr(runPos);
  assert(afterRun.find("if (done) {") != std::string::npos);
  // (les commentaires sont deja retires : on ne compare que du code)
  assert(afterRun.find("releaseUploadLock(request);") < afterRun.find("} else {"));
  assert(afterRun.find("_upload.lastActivity = millis() - UPLOAD_LOCK_TIMEOUT_MS - 1;")
         != std::string::npos);
}

// C-6 (GARDE) : le remplacement passe par la transaction, et plus par
// remove()+rename().
void guard_c6_midi_replacement_is_transactional() {
  const std::string web = finWebCodeOnly(finWebRead("Servo_flute_ESP32/WebConfigurator.cpp"));
  const std::string hdr = finWebRead("Servo_flute_ESP32/WebConfigurator.h");
  assert(!web.empty() && !hdr.empty());

  assert(web.find("#include \"FileTransaction.h\"") != std::string::npos);

  const std::string finalize = finWebSection(web, "case WEBOP_MIDI_FINALIZE", "case WEBOP_MIDI_LOAD");
  assert(!finalize.empty());
  // La validation du CONTENU precede toujours le remplacement (correctif deja
  // en place : on ne le defait pas).
  assert(finalize.find("_player->loadFile(_upload.tmpPath.c_str())") < finalize.find("fileTxInstall("));
  // L'installation est transactionnelle.
  assert(finalize.find("fileTxInstall(fsOps, _upload.tmpPath.c_str()") != std::string::npos);
  assert(finalize.find("midiBackupPathFor(_upload.fileName)") != std::string::npos);
  // Plus AUCUNE suppression de la destination avant le remplacement.
  assert(finalize.find("LittleFS.remove(destPath)") == std::string::npos);
  assert(finalize.find("LittleFS.rename(_upload.tmpPath, destPath)") == std::string::npos);
  // L'echec rend bien storage_error, et dit que l'ancien est preserve.
  assert(finalize.find("\"storage_error\"") != std::string::npos);
  assert(finalize.find("resp[\"preserved\"]") != std::string::npos);

  // Le .bak vit HORS de MIDI_DIR : c'est ce qui garantit qu'aucun residu
  // n'apparait dans /api/midi/list ni dans le quota, qui ne parcourent que
  // MIDI_DIR.
  assert(web.find("kMidiBakPrefix = \"/.mbk_\"") != std::string::npos);
  const std::string list = finWebSection(web, "void WebConfigurator::handleMidiList", "\n}\n");
  assert(!list.empty());
  assert(list.find("LittleFS.open(MIDI_DIR)") != std::string::npos);
  const std::string used = finWebSection(web, "size_t WebConfigurator::getMidiStorageUsed", "\n}\n");
  assert(!used.empty());
  assert(used.find("LittleFS.open(MIDI_DIR)") != std::string::npos);
  // Le temporaire d'upload est lui aussi a la racine (invariant existant).
  assert(web.find("String(\"/.up\")") != std::string::npos);

  // Le chemin de DEMARRAGE existe et tourne avant que les routes ne soient
  // servies.
  assert(hdr.find("void recoverInterruptedMidiInstalls();") != std::string::npos);
  const std::string begin = finWebSection(web, "void WebConfigurator::begin(", "void WebConfigurator::update()");
  assert(!begin.empty());
  assert(begin.find("recoverInterruptedMidiInstalls();") < begin.find("setupRoutes();"));
  assert(web.find("fileTxRecover(fsOps") != std::string::npos);
}

// C-2 / C-4 (GARDE) : le point d'appel corrige est bien celui qui posait
// probleme, et le hand-off passe desormais par le canal.
void guard_c2_c4_web_configurator_uses_the_extracted_modules() {
  const std::string web = finWebCodeOnly(finWebRead("Servo_flute_ESP32/WebConfigurator.cpp"));
  const std::string hdr = finWebRead("Servo_flute_ESP32/WebConfigurator.h");
  assert(!web.empty() && !hdr.empty());

  // C-2 : le candidat n'est plus construit par copie NON VERROUILLEE de cfg.
  const std::string finalize =
      finWebSection(web, "void WebConfigurator::handleApiConfigFinalize", "\n}\n");
  assert(!finalize.empty());
  assert(finalize.find("new (std::nothrow) RuntimeConfig(cfg)") == std::string::npos);
  assert(finalize.find("snapshotActiveConfig(*candidatePtr)") != std::string::npos);
  // Verrou refuse -> la reponse deja en place dans ce fichier, pas une nouvelle.
  assert(finalize.find("config_busy") != std::string::npos);
  assert(finalize.find("503") != std::string::npos);

  // Le SSID, seul champ chaine lu depuis AsyncTCP, passe par la copie bornee.
  assert(web.find("snapshotConfigString(configLockOps(), cfg.wifiSsid") != std::string::npos);

  // C-4 : plus de drapeaux volatile, le canal est branche sur de VRAIES
  // primitives.
  assert(hdr.find("WebOpChannel _opChannel;") != std::string::npos);
  const std::string run = finWebSection(web, "bool WebConfigurator::runOnLoop", "\n}\n");
  assert(!run.empty());
  assert(run.find("_opChannel.beginPublish(ticket)") < run.find("_opChannel.publish(ticket)"));
  assert(run.find("_opChannel.publish(ticket)") < run.find("_opChannel.awaitResult(ticket)"));
  assert(run.find("_opChannel.awaitResult(ticket)") < run.find("_opChannel.endPublish(ticket)"));
  // La charge utile n'est ecrite qu'APRES avoir obtenu la propriete.
  assert(run.find("_opChannel.beginPublish(ticket)") < run.find("_op = op;"));
  assert(run.find("_op = op;") < run.find("_opChannel.publish(ticket)"));

  const std::string service =
      finWebSection(web, "void WebConfigurator::servicePendingOp", "\n}\n");
  assert(!service.empty());
  assert(service.find("_opChannel.claim(taken)") != std::string::npos);
  assert(service.find("taken.execute") != std::string::npos);
  assert(service.find("_opChannel.complete(taken)") != std::string::npos);
  assert(service.find("releaseWebOp(_op);") < service.find("_opChannel.complete(taken)"));

  // LA REGLE A NE PAS CASSER : aucun chemin WebSocket ne bloque.
  const std::string ws =
      finWebSection(web, "void WebConfigurator::processWsMessage", "void WebConfigurator::broadcastStatus");
  assert(!ws.empty());
  assert(ws.find("runOnLoop") == std::string::npos);
  assert(ws.find("_opChannel") == std::string::npos);
  const std::string evt =
      finWebSection(web, "void WebConfigurator::onWsEvent", "void WebConfigurator::processWsMessage");
  assert(!evt.empty());
  assert(evt.find("runOnLoop") == std::string::npos);
  assert(evt.find("_opChannel") == std::string::npos);
}

}  // namespace

void fin_web_run_all_tests() {
  // C-4 : le hand-off, execute pour de vrai.
  web_op_nominal_round_trip();
  web_op_is_executed_exactly_once();
  web_op_sequence_distinguishes_successive_ops();
  web_op_stale_completion_is_not_taken_for_a_fresh_one();
  web_op_abandoned_caller_does_not_apply_later();
  web_op_abandoned_caller_cannot_steal_the_slot();
  web_op_result_published_at_the_deadline_is_still_collected();
  web_op_lock_discipline_is_balanced();
  web_op_unusable_channel_fails_closed();

  // C-3 : la demande non perdable.
  latched_request_is_never_lost_and_coalesces();
  latched_request_posted_during_handling_survives();
  latched_request_without_primitives_fails_closed();

  // C-2 : la copie coherente.
  snapshot_under_granted_lock_is_bit_exact();
  snapshot_under_refused_lock_copies_nothing();
  snapshot_rejects_unusable_ops();
  snapshot_string_is_always_terminated();
  snapshot_degenerate_cases_do_not_lock();

  // C-1 / C-5 / C-6 : gardes de SOURCE (voir l'avertissement en tete de
  // section) - ils completent, ils ne remplacent rien.
  guard_c1_every_session_end_is_preceded_by_a_guaranteed_safing();
  guard_c5_session_state_is_only_touched_through_its_primitives();
  guard_c6_midi_replacement_is_transactional();
  guard_c2_c4_web_configurator_uses_the_extracted_modules();
}

#ifdef STANDALONE_TEST_MAIN
int main() {
  fin_web_run_all_tests();
  std::cout << "fin_web tests passed\n";
  return 0;
}
#endif

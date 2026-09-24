#include "WebOpChannel.h"

namespace {

// Un jeu de primitives incomplet ferait planter le firmware au premier appel.
// Meme discipline que ConfigPersist / ConfigSnapshot : on refuse franchement,
// l'appelant HTTP repond "busy" plutot que de redemarrer la carte.
inline bool channelOpsUsable(const WebOpChannelOps& ops) {
  return ops.lockProducer != nullptr && ops.unlockProducer != nullptr &&
         ops.enterState != nullptr && ops.exitState != nullptr &&
         ops.waitDone != nullptr && ops.signalDone != nullptr &&
         ops.drainDone != nullptr && ops.nowMs != nullptr &&
         ops.yieldMs != nullptr;
}

}  // namespace

/* --- LatchedRequest ------------------------------------------------------- */

LatchedRequest::LatchedRequest()
  : _enter(nullptr), _exit(nullptr), _ctx(nullptr), _flag(false) {}

void LatchedRequest::begin(void (*enterCritical)(void*), void (*exitCritical)(void*),
                           void* ctx) {
  _enter = enterCritical;
  _exit = exitCritical;
  _ctx = ctx;
  _flag = false;
}

void LatchedRequest::request() {
  if (_enter == nullptr || _exit == nullptr) return;
  _enter(_ctx);
  _flag = true;
  _exit(_ctx);
}

bool LatchedRequest::take() {
  if (_enter == nullptr || _exit == nullptr) return false;
  // Lecture ET effacement dans LA MEME section critique. C'est tout ce qui
  // separe ce code du motif qui perdait des demandes.
  _enter(_ctx);
  const bool requested = _flag;
  _flag = false;
  _exit(_ctx);
  return requested;
}

bool LatchedRequest::pending() const {
  if (_enter == nullptr || _exit == nullptr) return false;
  _enter(_ctx);
  const bool requested = _flag;
  _exit(_ctx);
  return requested;
}

/* --- WebOpChannel --------------------------------------------------------- */

WebOpChannel::WebOpChannel()
  : _timeoutMs(0), _usable(false),
    _state(WEBOP_SLOT_IDLE), _seq(0), _doneSeq(0), _abandoned(false),
    _seqCounter(0) {
  _ops.lockProducer = nullptr;
  _ops.unlockProducer = nullptr;
  _ops.enterState = nullptr;
  _ops.exitState = nullptr;
  _ops.waitDone = nullptr;
  _ops.signalDone = nullptr;
  _ops.drainDone = nullptr;
  _ops.nowMs = nullptr;
  _ops.yieldMs = nullptr;
  _ops.ctx = nullptr;
}

void WebOpChannel::begin(const WebOpChannelOps& ops, uint32_t timeoutMs) {
  _ops = ops;
  _timeoutMs = timeoutMs;
  _usable = channelOpsUsable(ops);
  _state = WEBOP_SLOT_IDLE;
  _seq = 0;
  _doneSeq = 0;
  _abandoned = false;
  _seqCounter = 0;
}

/* --- Cote producteur ------------------------------------------------------ */

bool WebOpChannel::beginPublish(WebOpTicket& ticket) {
  ticket = WebOpTicket();
  if (!_usable) return false;

  if (!_ops.lockProducer(_ops.ctx, _timeoutMs)) return false;
  ticket.holdsProducerLock = true;

  // Attendre que l'emplacement redevienne libre. Il peut etre encore occupe par
  // une operation ABANDONNEE que loop() n'a pas fini de liberer : l'ecraser
  // ferait ecrire l'emplacement pendant que loop() le lit.
  const uint32_t deadline = _ops.nowMs(_ops.ctx) + _timeoutMs;
  while (true) {
    _ops.enterState(_ops.ctx);
    const bool slotFree = (_state == WEBOP_SLOT_IDLE);
    _ops.exitState(_ops.ctx);
    if (slotFree) break;
    if ((int32_t)(_ops.nowMs(_ops.ctx) - deadline) >= 0) {
      // Echec APRES la prise du verrou : on le relache ici et on efface le
      // jeton, pour qu'aucun chemin ne puisse le relacher une seconde fois.
      _ops.unlockProducer(_ops.ctx);
      ticket.holdsProducerLock = false;
      return false;
    }
    _ops.yieldMs(_ops.ctx, 1);
  }

  // Vider un signal de fin laisse par une operation precedente AVANT d'armer la
  // notre : sinon awaitResult() reviendrait immediatement sur un signal
  // etranger. Fait hors section critique - c'est une primitive de semaphore.
  _ops.drainDone(_ops.ctx);

  _ops.enterState(_ops.ctx);
  _seqCounter++;
  if (_seqCounter == 0) _seqCounter++;   // 0 est reserve a "aucune operation"
  _seq = _seqCounter;
  _abandoned = false;
  _ops.exitState(_ops.ctx);

  ticket.seq = _seq;
  return true;
}

void WebOpChannel::publish(WebOpTicket& ticket) {
  // Un jeton qui ne detient pas le verrou ne possede pas l'emplacement : il n'a
  // rien a y publier.
  if (!_usable || !ticket.holdsProducerLock || ticket.seq == 0) return;
  _ops.enterState(_ops.ctx);
  if (_state == WEBOP_SLOT_IDLE && _seq == ticket.seq) {
    _state = WEBOP_SLOT_ARMED;
  }
  _ops.exitState(_ops.ctx);
}

bool WebOpChannel::awaitResult(WebOpTicket& ticket) {
  if (!_usable || !ticket.holdsProducerLock || ticket.seq == 0) return false;

  const uint32_t deadline = _ops.nowMs(_ops.ctx) + _timeoutMs;
  while (true) {
    // Le resultat est-il DEJA la ? On le regarde avant d'attendre : le signal a
    // pu partir entre publish() et ici.
    //
    // La comparaison de sequence est REDONDANTE avec la machine a etats - un
    // emplacement DONE ne peut porter que l'operation du producteur qui tient
    // le verrou, puisque complete() libere tout de suite celles qu'on a
    // abandonnees. Elle est gardee comme seconde barriere : c'est exactement
    // cette confusion entre "un resultat" et "MON resultat" que l'ancien
    // hand-off rendait possible, et aucun test ne peut la faire echouer tant
    // que la machine a etats tient. Si un jour deux operations pouvaient se
    // chevaucher, ce test-ci serait la derniere chose qui tiendrait encore.
    _ops.enterState(_ops.ctx);
    const bool ready = (_state == WEBOP_SLOT_DONE && _doneSeq == ticket.seq);
    _ops.exitState(_ops.ctx);
    if (ready) {
      ticket.collected = true;
      return true;
    }

    const int32_t remaining = (int32_t)(deadline - _ops.nowMs(_ops.ctx));
    if (remaining <= 0) break;
    // La valeur de retour est volontairement ignoree : un signal recu ne prouve
    // pas que c'est LE notre (une operation precedente a pu le laisser), et une
    // echeance ne prouve pas qu'il n'est pas arrive. Seul l'etat fait foi, et
    // il est relu en haut de boucle.
    (void)_ops.waitDone(_ops.ctx, (uint32_t)remaining);
  }

  // Echeance depassee. L'abandon et la publication du resultat par loop() sont
  // pris dans LA MEME section critique : l'un des deux gagne, jamais les deux.
  // C'est ce qui rend impossible l'etat "resultat publie que plus personne
  // n'attend", dans lequel l'ancien code laissait l'emplacement occupe.
  _ops.enterState(_ops.ctx);
  const bool ready = (_state == WEBOP_SLOT_DONE && _doneSeq == ticket.seq);
  if (!ready && _seq == ticket.seq) _abandoned = true;
  _ops.exitState(_ops.ctx);

  ticket.collected = ready;
  return ready;
}

void WebOpChannel::endPublish(WebOpTicket& ticket) {
  if (!ticket.holdsProducerLock) return;   // rien de pris : rien a relacher

  if (_usable && ticket.collected) {
    // Le resultat a ete lu par son proprietaire : l'emplacement est libre.
    _ops.enterState(_ops.ctx);
    if (_state == WEBOP_SLOT_DONE && _doneSeq == ticket.seq) {
      _state = WEBOP_SLOT_IDLE;
    }
    _ops.exitState(_ops.ctx);
  }
  // Sinon l'operation est ARMED ou RUNNING et marquee abandonnee : c'est loop()
  // qui liberera l'emplacement quand elle aura fini d'en disposer. Le liberer
  // ICI ferait ecrire le prochain producteur dans une charge utile que loop()
  // est en train de lire - exactement la course que ce module supprime.

  if (_usable) _ops.unlockProducer(_ops.ctx);
  ticket.holdsProducerLock = false;
}

/* --- Cote consommateur (loop()) ------------------------------------------- */

bool WebOpChannel::claim(WebOpClaim& out) {
  out = WebOpClaim();
  if (!_usable) return false;

  _ops.enterState(_ops.ctx);
  const bool armed = (_state == WEBOP_SLOT_ARMED);
  if (armed) {
    // ARMED -> RUNNING est indivisible : une operation publiee est prise
    // exactement une fois, meme si servicePendingOp() etait appelee deux fois.
    _state = WEBOP_SLOT_RUNNING;
    out.seq = _seq;
    out.execute = !_abandoned;
  }
  _ops.exitState(_ops.ctx);
  return armed;
}

void WebOpChannel::complete(const WebOpClaim& taken) {
  if (!_usable || taken.seq == 0) return;

  bool signal = false;
  _ops.enterState(_ops.ctx);
  if (_state == WEBOP_SLOT_RUNNING && _seq == taken.seq) {
    _doneSeq = taken.seq;
    if (_abandoned) {
      // Personne n'attend ce resultat. On libere tout de suite et on ne laisse
      // AUCUN signal derriere : le prochain producteur ne doit pas le prendre
      // pour le sien. (Le drainDone() de beginPublish() est la ceinture ; ceci
      // sont les bretelles.)
      _state = WEBOP_SLOT_IDLE;
    } else {
      _state = WEBOP_SLOT_DONE;
      signal = true;
    }
  }
  _ops.exitState(_ops.ctx);

  // Le signal part HORS section critique : donner un semaphore peut reveiller
  // une tache, ce qui n'a rien a faire sous interruptions masquees.
  if (signal) _ops.signalDone(_ops.ctx);
}

/* --- Observation ---------------------------------------------------------- */

WebOpSlotState WebOpChannel::state() const {
  if (!_usable) return WEBOP_SLOT_IDLE;
  _ops.enterState(_ops.ctx);
  const WebOpSlotState s = _state;
  _ops.exitState(_ops.ctx);
  return s;
}

uint32_t WebOpChannel::lastDoneSeq() const {
  if (!_usable) return 0;
  _ops.enterState(_ops.ctx);
  const uint32_t s = _doneSeq;
  _ops.exitState(_ops.ctx);
  return s;
}

bool WebOpChannel::abandoned() const {
  if (!_usable) return false;
  _ops.enterState(_ops.ctx);
  const bool a = _abandoned;
  _ops.exitState(_ops.ctx);
  return a;
}

bool WebOpChannel::pending() const {
  const WebOpSlotState s = state();
  return s == WEBOP_SLOT_ARMED || s == WEBOP_SLOT_RUNNING;
}

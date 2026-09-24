#include "CommandQueue.h"
#include <new>   // std::nothrow : une allocation ratee doit rendre nullptr, pas lever

CommandQueue::CommandQueue(uint8_t capacity)
  : _items(nullptr), _capacity(capacity), _head(0), _tail(0), _count(0),
    _dropped(0), _panic(false), _stopRequests(0), _pendingPumpStops(0) {
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

bool CommandQueue::pushOrFallback(const ActuatorCommand& cmd) {
  portENTER_CRITICAL(&_mux);
  if (_items == nullptr || _count >= _capacity) {
    // PAS de _dropped++ ici, et c'est le seul point qui distingue cette methode
    // de push() : l'appelant bascule sur un canal qui ne peut pas echouer, donc
    // la commande n'est pas perdue. La compter comme perdue ferait accuser
    // dropped_commands d'une saturation sans consequence.
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
  // Les ordres d'ARRET en attente, eux, sont volontairement CONSERVES. Le panic
  // eteint deja tout, donc les appliquer ensuite ne fait rien ; mais les effacer
  // demanderait de prouver qu'aucun arret depose par une autre tache pendant le
  // panic ne se perd au passage. Conserver va dans le sens sur : un arret
  // applique en trop ne peut que retirer de l'energie, jamais en ajouter.
  portEXIT_CRITICAL(&_mux);
}

/*------------------------------------------------------------------------------
 * Ordres d'ARRET : meme discipline que le panic.
 *----------------------------------------------------------------------------*/

bool CommandQueue::commandEnergizes(const ActuatorCommand& cmd, uint8_t bits) {
  switch (cmd.type) {
    // Consignes de pompe : elles n'alimentent que si elles demandent autre chose
    // que zero. Une consigne a 0 % laissee en file est inoffensive.
    // Une consigne a zero purge les consignes NON NULLES deja en file, pour la
    // meme raison qu'un arret : sinon la plus ancienne s'appliquerait APRES elle
    // et realimenterait la pompe.
    case ACMD_PUMP_TARGET:
      return (bits & (STOPREQ_PUMPS | STOPREQ_PUMP_TARGET_ZERO)) != 0 && cmd.b > 0;
    // STOPREQ_PUMP_TARGET_ZERO est volontairement ABSENT ici : une consigne
    // globale a zero ne termine pas un test mono-pompe (setTargetPercent(0) ne
    // touche pas _testPumpIndex), seul l'arret dur le fait.
    case ACMD_PUMP_SINGLE_TEST: return (bits & STOPREQ_PUMPS) != 0 && cmd.b > 0;
    // Seul pump_enable = true defait pump_enable = false.
    case ACMD_PUMP_ENABLE:      return (bits & STOPREQ_PUMPS_OFF) != 0 && cmd.a != 0;
    case ACMD_FAN_TARGET:
      return (bits & (STOPREQ_FAN | STOPREQ_FAN_TARGET_ZERO)) != 0 && cmd.b > 0;
    // Seule l'OUVERTURE de la valve defait sa fermeture.
    case ACMD_TEST_SOLENOID:    return (bits & STOPREQ_SOLENOID) != 0 && cmd.a != 0;
    default:                    return false;
  }
}

void CommandQueue::purgeEnergizingLocked(uint8_t bits, bool allPumpTests) {
  if (_items == nullptr || _count == 0) return;
  // Compactage EN PLACE dans l'anneau circulaire : `write` ne depasse jamais
  // `read`, donc on n'ecrit que sur des emplacements deja lus.
  uint8_t read = _tail;
  uint8_t write = _tail;
  uint8_t kept = 0;
  const uint8_t total = _count;
  for (uint8_t i = 0; i < total; i++) {
    const ActuatorCommand c = _items[read];
    const bool drop = commandEnergizes(c, bits) ||
                      (allPumpTests && c.type == ACMD_PUMP_SINGLE_TEST && c.b > 0);
    if (!drop) {
      if (write != read) _items[write] = c;
      write = (uint8_t)((write + 1) % _capacity);
      kept++;
    }
    read = (uint8_t)((read + 1) % _capacity);
  }
  _count = kept;
  _head = write;
}

void CommandQueue::requestStop(uint8_t bits) {
  if (bits == 0) return;
  portENTER_CRITICAL(&_mux);
  // Coalescence : dix demandes identiques posent le meme bit, et la prise les
  // ramasse toutes en une application. Jamais zero, jamais dix.
  _stopRequests |= bits;
  // MEME RAISONNEMENT QUE requestPanic(), a portee reduite. Le panic VIDE
  // l'anneau pour qu'aucune commande emise avant lui ne s'applique apres lui ;
  // un arret cible n'en retire que ce qui REALIMENTERAIT l'actionneur vise, afin
  // de ne pas jeter au passage des commandes sans rapport (notes, doigts, angles
  // de servo). Sans cette purge, une consigne de pompe deja en file s'appliquait
  // APRES l'arret et relancait la pompe - exactement le "on finit alimente" que
  // ce canal existe pour empecher.
  purgeEnergizingLocked(bits, false);
  portEXIT_CRITICAL(&_mux);
}

uint8_t CommandQueue::takeStopRequests() {
  portENTER_CRITICAL(&_mux);
  uint8_t bits = _stopRequests;
  _stopRequests = 0;
  portEXIT_CRITICAL(&_mux);
  return bits;
}

bool CommandQueue::stopRequestsPending() const {
  portENTER_CRITICAL(&_mux);
  bool any = _stopRequests != 0;
  portEXIT_CRITICAL(&_mux);
  return any;
}

void CommandQueue::requestPumpStopSingle(uint8_t pumpIndex) {
  if (pumpIndex >= COMMAND_QUEUE_MAX_PUMP_STOPS) return;
  portENTER_CRITICAL(&_mux);
  _pendingPumpStops |= (uint8_t)(1u << pumpIndex);
  // L'arret d'un test mono-pompe est GLOBAL cote controleur
  // (stopSinglePumpTest() ne prend pas d'index) : on retire donc de l'anneau
  // TOUS les tests mono-pompe en attente, pas seulement celui de `pumpIndex`.
  // Ne retirer que l'index demande laisserait le test d'une autre pompe demarrer
  // juste apres l'arret, ce qui est precisement le cas "on finit alimente".
  purgeEnergizingLocked(0, true);
  portEXIT_CRITICAL(&_mux);
}

bool CommandQueue::takePendingPumpStop(uint8_t& pumpIndex) {
  bool found = false;
  portENTER_CRITICAL(&_mux);
  for (uint8_t i = 0; i < COMMAND_QUEUE_MAX_PUMP_STOPS; i++) {
    if (_pendingPumpStops & (uint8_t)(1u << i)) {
      _pendingPumpStops &= (uint8_t)~(1u << i);
      pumpIndex = i;
      found = true;
      break;
    }
  }
  portEXIT_CRITICAL(&_mux);
  return found;
}

bool CommandQueue::hasPendingPumpStop() const {
  portENTER_CRITICAL(&_mux);
  bool any = _pendingPumpStops != 0;
  portEXIT_CRITICAL(&_mux);
  return any;
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
  // TROIS APPELANTS, et chacun a deja eteint la note en cours avant d'arriver
  // ici : le demarrage (rien ne joue), allSoundOff() (qui eteint tout), et la
  // prise de possession par le calibrateur (setActuatorSessionActive(true), qui
  // appelle _sequencer.stop() - donc closeSolenoid() + setAirflowToRest() -
  // juste avant). Un Note Off encore en attente n'a donc plus d'objet dans
  // aucun des trois cas : il serait applique sur une note qui ne joue plus, et
  // pendant une session noteOff() sort de toute facon immediatement.
  for (uint8_t i = 0; i < 4; i++) _pendingNoteOff[i] = 0;
  // Les ordres d'arret sont CONSERVES, pour la meme raison que dans
  // requestPanic() : les appliquer en trop ne peut que retirer de l'energie.
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

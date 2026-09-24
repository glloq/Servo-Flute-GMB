/***********************************************************************************************
 * WebOpChannel - Hand-off SYNCHRONISE dans les deux sens, AsyncTCP <-> loop()
 *
 * LE DEFAUT CORRIGE
 * -----------------
 * Le hand-off de WebConfigurator reposait sur trois variables `volatile` :
 *
 *     volatile bool     _opPending;
 *     volatile bool     _opAbandoned;
 *     volatile uint32_t _opDoneSeq;
 *
 * `volatile` interdit au compilateur de mettre une variable en cache. Il
 * n'apporte NI atomicite, NI barriere memoire, et n'est pas une primitive de
 * synchronisation inter-coeur. La meme correction a deja ete faite dans
 * `InstrumentManager` (`_requestMux`, `takePowerOnRequest()`,
 * `takeResetControllersRequest()`) : ce module prolonge ce motif.
 *
 * Concretement, le transfert n'etait synchronise que dans UN sens : une tache
 * ECRIVAIT l'emplacement `_op` pendant que l'autre se contentait de surveiller
 * un booleen. Les trois transitions qui comptent - "publie", "pris", "termine" -
 * etaient des ecritures independantes, donc observables dans le desordre, et
 * deux d'entre elles portaient une decision (abandonner / signaler) prise a
 * partir d'une lecture faite AILLEURS. En particulier, un appelant pouvait
 * declarer son operation abandonnee a l'instant meme ou loop() publiait le
 * resultat : le resultat existait, personne ne le lisait, et l'emplacement
 * restait occupe.
 *
 * CE QUE FAIT CE MODULE
 * ---------------------
 * Il ne transporte PAS la charge utile. La charge utile (`WebOp`, qui porte des
 * `String` Arduino) reste dans WebConfigurator ; ce module possede l'ETAT du
 * transfert et dit, a chaque instant, QUI a le droit de toucher l'emplacement :
 *
 *   IDLE     personne ne possede l'emplacement ; un producteur peut le prendre
 *   ARMED    le producteur a publie ; l'emplacement appartient a loop()
 *   RUNNING  loop() execute ; l'emplacement lui appartient
 *   DONE     le resultat est publie ; il appartient au producteur qui attend
 *
 * Chaque transition est faite dans UNE section critique (les primitives sont
 * injectees), ce qui rend indivisibles les deux decisions qui s'excluent :
 * "j'abandonne" (producteur) et "je publie le resultat" (loop()). L'une des
 * deux gagne, jamais les deux, donc l'emplacement ne peut ni rester occupe sans
 * proprietaire ni etre libere sous les pieds de celui qui le lit.
 *
 * POURQUOI PAS UNE FILE FREERTOS
 * ------------------------------
 * Il y a UN seul emplacement par conception (un producteur HTTP a la fois,
 * serialise par le verrou de producteur). Une file n'ajouterait que de
 * l'ordonnancement dont personne n'a besoin, et couterait une allocation par
 * operation pour une charge utile qui porte des String.
 *
 * REGLE QUE CE MODULE NE CHANGE PAS
 * ---------------------------------
 * Seul le chemin HTTP ATTEND. Les commandes WebSocket restent non bloquantes
 * (file `_wsOps`), parce qu'un callback WebSocket peut detenir le verrou
 * interne d'AsyncWebSocket que loop() reprend pour diffuser un statut : une
 * attente croisee bloquerait les deux taches. Ce module ne fournit aucun moyen
 * pour un chemin WS d'attendre.
 *
 * CE QU'UN TEST HOTE PROUVE, ET CE QU'IL NE PROUVE PAS
 * ----------------------------------------------------
 * Les sections critiques sont des no-ops dans le stub hote et le test est
 * mono-tache : on prouve le CONTRAT de la machine a etats (execution unique,
 * resultat rendu a SON appelant, abandon sans effet de bord, comptage exact des
 * verrous), PAS l'absence de course.
 ***********************************************************************************************/
#ifndef WEB_OP_CHANNEL_H
#define WEB_OP_CHANNEL_H

#include <stdint.h>

/***********************************************************************************************
 * LatchedRequest - une demande deposee par une tache, PRISE par une autre
 *
 * `volatile bool flag;` + `if (flag) { flag = false; ... }` est le motif qui
 * PERD des demandes : la lecture et l'effacement sont deux acces distincts, et
 * une demande deposee entre les deux disparait sans avoir ete traitee. Sur
 * l'annulation de calibration, cela voulait dire : panic recu, actionneurs
 * coupes, puis AutoCalibrator - qui se croyait toujours "running" - rouvrait la
 * valve au pas suivant.
 *
 * Contrat, identique a CommandQueue::takePanicRequest() :
 *   - `request()` ne peut jamais etre perdue ;
 *   - `take()` lit ET efface INDIVISIBLEMENT ;
 *   - deux `request()` coalescent en un seul `take()` vrai (on traite une fois,
 *     pas deux) ;
 *   - une `request()` deposee PENDANT le traitement de la precedente survit a
 *     ce traitement : elle sera prise au tour suivant.
 *
 * La section critique est injectee (portMUX cote ESP32, no-op cote hote).
 **********************************************************************************************/
class LatchedRequest {
public:
  LatchedRequest();
  // Sans begin(), l'objet est INUTILISABLE : request() ne pose rien et take()
  // rend false. Echouer en fermeture plutot que de manipuler un drapeau qui
  // n'est protege par rien.
  void begin(void (*enterCritical)(void*), void (*exitCritical)(void*), void* ctx);
  void request();
  bool take();
  bool pending() const;

private:
  void (*_enter)(void*);
  void (*_exit)(void*);
  void* _ctx;
  bool _flag;
};

// Etat de l'unique emplacement. Il dit qui possede la charge utile.
enum WebOpSlotState : uint8_t {
  WEBOP_SLOT_IDLE = 0,
  WEBOP_SLOT_ARMED,
  WEBOP_SLOT_RUNNING,
  WEBOP_SLOT_DONE
};

// Primitives injectees. Deux verrous DISTINCTS, et c'est delibere :
//
//  - `lockProducer` / `unlockProducer` : un mutex bloquant qui serialise les
//    producteurs entre eux. Il peut etre tenu longtemps (toute la duree de
//    l'operation) et autorise l'attente ;
//  - `enterState` / `exitState` : une section critique MINUSCULE qui protege
//    les quelques champs d'etat ci-dessous. Sur ESP32 c'est un portMUX, donc
//    interruptions masquees : on n'y alloue rien, on n'y appelle rien.
//
// Les melanger serait faux dans les deux sens : tenir un portMUX pendant une
// operation de plusieurs secondes fige le coeur, et serialiser les transitions
// d'etat avec un mutex bloquant les rendrait impossibles a prendre depuis
// loop() pendant qu'un producteur attend.
struct WebOpChannelOps {
  bool     (*lockProducer)(void* ctx, uint32_t timeoutMs);
  void     (*unlockProducer)(void* ctx);
  void     (*enterState)(void* ctx);
  void     (*exitState)(void* ctx);
  // Attente/signal de fin d'operation (semaphore binaire cote ESP32).
  // `waitDone` rend true si le signal a ete recu, false sur echeance.
  bool     (*waitDone)(void* ctx, uint32_t timeoutMs);
  void     (*signalDone)(void* ctx);
  // Consomme un signal en retard sans attendre. Sans lui, l'attente d'une
  // operation retournerait immediatement sur le signal d'une operation
  // precedente qui a ete abandonnee.
  void     (*drainDone)(void* ctx);
  uint32_t (*nowMs)(void* ctx);
  void     (*yieldMs)(void* ctx, uint32_t ms);
  void* ctx;
};

// Jeton du producteur. Il porte le numero de sequence de SON operation et le
// fait qu'il detient encore le verrou de producteur : c'est ce qui rend
// impossible de relacher un verrou qu'on n'a pas pris.
struct WebOpTicket {
  uint32_t seq;
  bool holdsProducerLock;
  bool collected;
  WebOpTicket() : seq(0), holdsProducerLock(false), collected(false) {}
};

// Jeton du consommateur (loop()).
struct WebOpClaim {
  uint32_t seq;
  bool execute;    // false : l'appelant a deja renonce, ne pas appliquer
  WebOpClaim() : seq(0), execute(false) {}
};

class WebOpChannel {
public:
  WebOpChannel();

  // `timeoutMs` borne a la fois l'attente du verrou de producteur, l'attente de
  // liberation de l'emplacement et l'attente du resultat.
  void begin(const WebOpChannelOps& ops, uint32_t timeoutMs);

  // ---- Cote producteur (tache AsyncTCP, chemin HTTP uniquement) ------------

  // Prend le verrou de producteur et attend que l'emplacement soit libre.
  // true  : le jeton est arme, l'appelant peut ECRIRE la charge utile, et il
  //         DOIT appeler endPublish() ;
  // false : rien n'est pris, rien n'est a relacher, l'appelant garde la
  //         propriete de ce qu'il portait.
  bool beginPublish(WebOpTicket& ticket);

  // Rend la charge utile visible pour loop(). A appeler APRES l'avoir ecrite.
  void publish(WebOpTicket& ticket);

  // Attend le resultat de CETTE operation (numero de sequence verifie).
  // true  : le resultat est dans l'emplacement, l'appelant peut le LIRE ;
  // false : echeance depassee, l'operation est marquee abandonnee - loop()
  //         ne l'appliquera pas si elle n'a pas commence, et liberera
  //         l'emplacement quand elle aura fini si elle a commence.
  bool awaitResult(WebOpTicket& ticket);

  // Relache le verrou de producteur, et l'emplacement si le resultat a ete
  // recupere. Idempotent : un jeton qui ne detient rien ne relache rien.
  void endPublish(WebOpTicket& ticket);

  // ---- Cote consommateur (tache loop()) ------------------------------------

  // Prend l'operation publiee, s'il y en a une. Le passage ARMED -> RUNNING est
  // indivisible, donc une operation publiee est prise EXACTEMENT une fois.
  // `out.execute` est faux si l'appelant a deja renonce : la charge utile est
  // alors a liberer sans l'appliquer.
  bool claim(WebOpClaim& out);

  // Publie la fin de l'operation. Si l'appelant attend encore, l'emplacement
  // passe en DONE et le signal part ; s'il a renonce, l'emplacement redevient
  // libre immediatement et aucun signal n'est laisse derriere.
  void complete(const WebOpClaim& taken);

  // ---- Observation (tests, diagnostics) ------------------------------------
  WebOpSlotState state() const;
  uint32_t lastDoneSeq() const;
  bool abandoned() const;
  // Vrai des qu'une operation est publiee et pas encore terminee. Remplace la
  // lecture directe de l'ancien `_opPending`.
  bool pending() const;

private:
  WebOpChannelOps _ops;
  uint32_t _timeoutMs;
  bool _usable;

  // --- Etat protege par enterState()/exitState() ----------------------------
  // Aucune de ces variables n'est `volatile` : ce n'est pas `volatile` qui les
  // rend sures, c'est la section critique. Les declarer volatile en plus
  // donnerait l'illusion d'une garantie supplementaire qui n'existe pas.
  WebOpSlotState _state;
  uint32_t _seq;        // sequence de l'operation presente dans l'emplacement
  uint32_t _doneSeq;    // sequence de la derniere operation terminee
  bool _abandoned;      // l'appelant de _seq a renonce

  // Compteur de sequence : touche par les producteurs seulement, qui sont
  // serialises par le verrou de producteur.
  uint32_t _seqCounter;
};

#endif

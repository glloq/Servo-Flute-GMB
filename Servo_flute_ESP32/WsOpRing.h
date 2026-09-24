/***********************************************************************************************
 * WsOpRing - la comptabilite d'indices de la file d'operations WebSocket, et la
 * BORNE de travail par passe de loop().
 *
 * LE DEFAUT CORRIGE
 * -----------------
 * `WebConfigurator::serviceWsOps()` drainait la file ENTIEREMENT :
 *
 *     void WebConfigurator::serviceWsOps() {
 *       while (true) { ... executeWebOp(op); ... }
 *     }
 *
 * La file ne contient que six places, ce qui parait peu - mais ces six-la ne
 * sont pas equivalentes. `WEBOP_MIC_RESET` appelle `resetMicrophone()`, qui
 * comporte un `delay(100)` et jusqu'a ~500 ms d'attente I2S ; le chargement
 * d'un fichier MIDI, un commit de configuration et les acces LittleFS sont du
 * meme ordre. Six operations de cette famille dans une seule passe retiennent
 * `loop()` pendant une duree proche du plafond du chien de garde (4 s) - et
 * surtout repoussent d'autant `InstrumentManager::update()`, qui est l'endroit
 * ou les ordres d'ARRET et le PANIC sont reellement appliques aux actionneurs.
 *
 * Autrement dit : plus le plan de controle web est charge, plus la mise en
 * securite tarde. C'est l'inverse de ce qu'on veut.
 *
 * POURQUOI UN MODULE
 * ------------------
 * `WebConfigurator.cpp` n'est compilable par aucun build hote. En sortir les
 * indices - et rien d'autre : la charge utile `WebOp`, qui porte des `String`
 * Arduino, reste dans le tableau de WebConfigurator - rend la propriete
 * interessante reellement executable en test : six operations en file, une
 * seule executee par passe, les cinq autres intactes, dans l'ordre, jamais
 * perdues.
 *
 * CE QUE CE MODULE NE CHANGE PAS
 * ------------------------------
 * Le panic et l'annulation de calibration n'empruntent PAS cette file : ils ont
 * chacun leur drapeau dedie, imperdable et consomme ailleurs. Borner le
 * drainage ne peut donc pas retarder une mise en securite - c'est meme tout
 * l'objet de la borne.
 ***********************************************************************************************/
#ifndef WS_OP_RING_H
#define WS_OP_RING_H

#include <stdint.h>

// UNE operation par passe de loop(). Le chiffre n'est pas un reglage prudent
// pris au hasard : c'est la seule valeur pour laquelle la latence ajoutee a
// InstrumentManager::update() est bornee par la PLUS LONGUE operation web, et
// non par leur somme. Avec six places et une seule operation lente, une passe
// coute au pire ~600 ms au lieu de ~3,6 s.
//
// Ce que cela coute : six operations web mises en file d'un coup mettent six
// passes de loop() a s'ecouler. Une passe dure quelques millisecondes hors
// operation lente, donc l'utilisateur ne le voit pas ; et aucune n'est perdue.
static const uint8_t WS_OP_MAX_PER_PASS = 1;

class WsOpRing {
 public:
  WsOpRing(uint8_t capacity, uint8_t maxPerPass);

  // Reserve la prochaine place d'ecriture. False = file pleine (l'appelant
  // refuse l'operation, comme avant).
  bool push(uint8_t& slot);

  // Rend la prochaine place a executer, SI la borne de la passe le permet.
  // False = file vide OU borne atteinte. Dans le second cas l'operation reste
  // en file, a sa place, et sortira a la passe suivante.
  bool popForPass(uint8_t& slot);

  // A appeler au debut de chaque passe de loop() : remet a zero le compteur
  // d'operations executees dans la passe.
  void beginPass();

  uint8_t count() const { return _count; }
  uint8_t capacity() const { return _capacity; }
  uint8_t maxPerPass() const { return _maxPerPass; }
  bool full() const { return _count >= _capacity; }
  uint8_t ranThisPass() const { return _ranThisPass; }

 private:
  uint8_t _capacity;
  uint8_t _maxPerPass;
  uint8_t _head;
  uint8_t _tail;
  uint8_t _count;
  uint8_t _ranThisPass;
};

#endif

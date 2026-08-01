/***********************************************************************************************
 * MidiTempoMap - Carte globale tick -> tempo pour la lecture des SMF Type 1
 *
 * Probleme corrige :
 *   Dans un SMF Type 1, les changements de tempo vivent presque toujours dans la
 *   piste 0 (piste de tempo), tandis que les notes vivent dans les pistes 1+.
 *   Si chaque piste est convertie en millisecondes independamment avec un tempo
 *   reinitialise a 120 BPM, les pistes de notes ignorent totalement la piste de
 *   tempo et se desynchronisent des le premier changement de tempo.
 *
 * Solution :
 *   1. On collecte TOUS les changements de tempo de TOUTES les pistes (en ticks
 *      absolus) dans une seule carte globale.
 *   2. finalize() trie la carte par tick, fusionne les doublons (dernier gagne)
 *      et precalcule le temps cumule en millisecondes a chaque point de rupture.
 *   3. tickToMs() convertit n'importe quel tick absolu (donc n'importe quel
 *      evenement de n'importe quelle piste) en utilisant le bon segment de tempo.
 *
 * Header-only et sans dependance materielle : entierement testable a l'hote.
 ***********************************************************************************************/
#ifndef MIDI_TEMPO_MAP_H
#define MIDI_TEMPO_MAP_H

#include <Arduino.h>

// Nombre max de changements de tempo suivis (entree 0 = tempo par defaut a t=0).
// 64 couvre tres largement les morceaux reels ; au-dela on signale un overflow.
#ifndef MIDI_MAX_TEMPO_CHANGES
#define MIDI_MAX_TEMPO_CHANGES 64
#endif

class MidiTempoMap {
public:
  MidiTempoMap() { begin(480); }

  // (Re)initialise la carte. division = ticks par noire (PPQN), issu du MThd.
  // Une entree implicite {tick=0, tempo=500000us (120 BPM)} est toujours posee
  // en tete pour couvrir la periode avant le premier changement de tempo.
  void begin(uint16_t division) {
    _division = (division == 0) ? 1 : division;
    _count = 1;
    _tick[0] = 0;
    _tempo[0] = 500000;  // 120 BPM par defaut
    _cumMs[0] = 0;
    _finalized = false;
    _overflow = false;
  }

  // Enregistre un changement de tempo a un tick absolu (us par noire).
  // Retourne false si la carte est pleine (overflow signale) ; les tempos nuls
  // (fichier corrompu) sont ignores silencieusement. A appeler pendant le
  // parsing, avant finalize().
  bool addTempoChange(uint32_t tick, uint32_t tempo) {
    if (_finalized || tempo == 0) return _finalized ? false : true;
    if (_count >= MIDI_MAX_TEMPO_CHANGES) {
      _overflow = true;
      return false;
    }
    _tick[_count] = tick;
    _tempo[_count] = tempo;
    _cumMs[_count] = 0;
    _count++;
    return true;
  }

  // La carte a-t-elle recu plus de MIDI_MAX_TEMPO_CHANGES changements ?
  bool overflowed() const { return _overflow; }

  // Trie par tick (tri stable), fusionne les changements au meme tick (le
  // dernier rencontre l'emporte, conforme a la semantique SMF) et precalcule
  // les millisecondes cumulees a chaque point de rupture.
  void finalize() {
    if (_finalized) return;

    // Tri par insertion stable sur le tick : a tick egal, l'ordre d'insertion
    // (donc l'ordre de lecture des pistes) est preserve.
    for (uint16_t i = 1; i < _count; i++) {
      uint32_t tk = _tick[i];
      uint32_t tp = _tempo[i];
      int32_t j = (int32_t)i - 1;
      while (j >= 0 && _tick[j] > tk) {
        _tick[j + 1] = _tick[j];
        _tempo[j + 1] = _tempo[j];
        j--;
      }
      _tick[j + 1] = tk;
      _tempo[j + 1] = tp;
    }

    // Fusion des doublons de tick : on garde le dernier tempo vu (indice le plus
    // eleve apres tri stable = dernier insere a ce tick).
    uint16_t w = 0;
    for (uint16_t r = 0; r < _count; r++) {
      if (w > 0 && _tick[w - 1] == _tick[r]) {
        _tempo[w - 1] = _tempo[r];
      } else {
        _tick[w] = _tick[r];
        _tempo[w] = _tempo[r];
        w++;
      }
    }
    _count = w;

    // Millisecondes cumulees a chaque changement de tempo.
    _cumMs[0] = 0;
    for (uint16_t i = 1; i < _count; i++) {
      uint32_t dTicks = _tick[i] - _tick[i - 1];
      _cumMs[i] = _cumMs[i - 1] + segmentMs(dTicks, _tempo[i - 1]);
    }

    _finalized = true;
  }

  // Convertit un tick absolu en millisecondes en appliquant le tempo du segment
  // qui contient ce tick. finalize() doit avoir ete appele.
  uint32_t tickToMs(uint32_t tick) const {
    uint16_t seg = 0;
    for (uint16_t i = 0; i < _count; i++) {
      if (_tick[i] <= tick) {
        seg = i;
      } else {
        break;  // carte triee : plus rien ne peut convenir
      }
    }
    return _cumMs[seg] + segmentMs(tick - _tick[seg], _tempo[seg]);
  }

  // Nombre de segments de tempo distincts (>= 1, l'entree par defaut comptant).
  uint16_t changeCount() const { return _count; }

private:
  // ms = ticks * (us_par_noire / ticks_par_noire) / 1000, en 64 bits pour
  // eviter tout overflow intermediaire.
  uint32_t segmentMs(uint32_t ticks, uint32_t tempo) const {
    return (uint32_t)((uint64_t)ticks * tempo / _division / 1000);
  }

  uint32_t _tick[MIDI_MAX_TEMPO_CHANGES];
  uint32_t _tempo[MIDI_MAX_TEMPO_CHANGES];
  uint32_t _cumMs[MIDI_MAX_TEMPO_CHANGES];
  uint16_t _count;
  uint16_t _division;
  bool _finalized;
  bool _overflow;
};

#endif

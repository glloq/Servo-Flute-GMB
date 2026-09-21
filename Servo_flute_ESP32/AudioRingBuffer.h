/***********************************************************************************************
 * AudioRingBuffer - Anneau d'echantillons audio continu, sans allocation
 *
 * PROBLEME RESOLU (audit PHASE 0, defauts A0-1 et A0-2)
 * -----------------------------------------------------
 * L'acquisition precedente lisait l'I2S avec un timeout nul et considerait que
 * TOUT ce que le DMA contenait a cet instant constituait une frame d'analyse :
 *
 *     _validSamples = bytesRead / sizeof(int32_t);   // 300 ? 900 ? peu importe
 *
 * Deux consequences. Le RMS etait calcule sur une fenetre de longueur inconnue,
 * donc le niveau dependait du hasard de l'ordonnancement plutot que du son. Et
 * sous ~322 echantillons, YIN sortait sans rien dire (W < tauMax + 1), alors que
 * le compteur de frames etait incremente quand meme : le consommateur recevait
 * une frame FRAICHE portant une mesure vide. La garde d'obsolescence ne pouvait
 * pas l'attraper, puisque la frame n'etait pas perimee - elle etait fausse.
 *
 * Par ailleurs le DMA ne contient que 32 ms alors que l'analyse tournait toutes
 * les 40 ms : 8 ms d'echantillons etaient ecrases a chaque cycle, et ce trou
 * pouvait tomber exactement sur une attaque de note.
 *
 * MODELE
 * ------
 * Le DMA alimente l'anneau aussi souvent que possible ; l'analyse n'extrait une
 * frame que lorsque FRAME_SIZE echantillons sont REELLEMENT disponibles. Une
 * frame est donc toujours complete, toujours contigue, et de longueur connue.
 *
 * Le pas d'avancement (hop) est distinct de la taille de frame, ce qui donne un
 * recouvrement :
 *
 *     frame 0 : 0    .. 1023
 *     frame 1 : 512  .. 1535      (hop = 512, recouvrement 50 %)
 *     frame 2 : 1024 .. 2047
 *
 * POLITIQUE DE DEBORDEMENT
 * ------------------------
 * Si l'ecriture depasse la place libre, ce sont les echantillons les PLUS
 * ANCIENS qui sont abandonnes, jamais les plus recents : pour un moniteur temps
 * reel, du son vieux de 100 ms n'a plus d'interet, alors que le son present en a.
 * Chaque perte est comptee (`droppedSamples`), de sorte qu'un debordement ne
 * puisse pas passer inapercu.
 *
 * Aucune allocation dynamique, aucune dependance Arduino / I2S : la classe se
 * teste entierement sur hote.
 ***********************************************************************************************/
#ifndef AUDIO_RING_BUFFER_H
#define AUDIO_RING_BUFFER_H

#include <stddef.h>
#include <stdint.h>
#include "settings.h"

// Compteurs de diagnostic de l'acquisition. Exposes par /api/diagnostics : un
// microphone qui "marche" mais accumule des debordements est un microphone dont
// les mesures ne valent rien, et cela doit se voir.
struct AudioCaptureStats {
  uint32_t samplesReceived = 0;    // echantillons entres dans l'anneau
  uint32_t framesProduced = 0;     // frames completes extraites
  uint32_t partialReads = 0;       // lectures I2S plus courtes que demandees
  uint32_t readErrors = 0;         // lectures I2S en erreur
  uint32_t bufferOverruns = 0;     // ecritures ayant du ecraser des echantillons
  uint32_t bufferUnderruns = 0;    // frame demandee sans assez d'echantillons
  uint32_t droppedSamples = 0;     // total d'echantillons perdus par debordement
  unsigned long lastFrameTimestamp = 0;

  void reset() { *this = AudioCaptureStats(); }
};

class AudioRingBuffer {
public:
  AudioRingBuffer() { reset(); }

  void reset();

  // Capacite utile (echantillons). Puissance de deux : l'indexation se fait par
  // masquage, sans modulo.
  static constexpr size_t capacity() { return MIC_RING_CAPACITY; }

  size_t available() const { return _count; }
  size_t freeSpace() const { return MIC_RING_CAPACITY - _count; }

  // Ecrit n echantillons. Retourne le nombre d'echantillons ABANDONNES (les plus
  // anciens) pour faire de la place ; 0 en fonctionnement normal. Une ecriture
  // plus grande que la capacite ne conserve que les derniers echantillons.
  size_t write(const float* src, size_t n, AudioCaptureStats* stats = nullptr);

  // Extrait une frame de `frameSize` echantillons contigus dans `out`, puis
  // avance la lecture de `hop`. Retourne false sans rien copier si moins de
  // `frameSize` echantillons sont disponibles : une frame partielle n'est
  // JAMAIS produite.
  bool readFrame(float* out, size_t frameSize, size_t hop, AudioCaptureStats* stats = nullptr);

  // Vrai si une frame complete peut etre extraite maintenant.
  bool frameReady(size_t frameSize) const { return _count >= frameSize; }

private:
  float _buf[MIC_RING_CAPACITY];
  size_t _readIdx;
  size_t _count;
};

#endif  // AUDIO_RING_BUFFER_H

#include "AudioRingBuffer.h"

#include <string.h>

// La capacite doit etre une puissance de deux : l'indexation se fait par
// masquage. Verifie a la compilation plutot qu'espere.
static_assert((MIC_RING_CAPACITY & (MIC_RING_CAPACITY - 1)) == 0,
              "MIC_RING_CAPACITY doit etre une puissance de deux");
// Une frame doit tenir dans l'anneau, sinon aucune frame ne serait jamais prete.
static_assert(MIC_RING_CAPACITY >= MIC_ANALYSIS_FRAME_SIZE,
              "MIC_RING_CAPACITY doit contenir au moins une frame d'analyse");
// PitchDetector borne n a MIC_BUFFER_SIZE et sa fenetre de Hann fait cette
// taille : une frame plus grande serait tronquee EN SILENCE.
static_assert(MIC_ANALYSIS_FRAME_SIZE <= MIC_BUFFER_SIZE,
              "MIC_ANALYSIS_FRAME_SIZE ne peut pas depasser MIC_BUFFER_SIZE");
// Un hop nul ou superieur a la frame n'a pas de sens : la lecture n'avancerait
// pas, ou sauterait des echantillons jamais analyses.
static_assert(MIC_ANALYSIS_HOP_SIZE >= 1 && MIC_ANALYSIS_HOP_SIZE <= MIC_ANALYSIS_FRAME_SIZE,
              "MIC_ANALYSIS_HOP_SIZE doit etre dans 1..MIC_ANALYSIS_FRAME_SIZE");

namespace {
constexpr size_t kMask = MIC_RING_CAPACITY - 1;
}

void AudioRingBuffer::reset() {
  _readIdx = 0;
  _count = 0;
  // Pas de memset du tableau : seuls les `_count` premiers echantillons sont
  // lisibles, et ils sont toujours ecrits avant d'etre lus. Eviter le memset
  // epargne 8 ko d'ecriture a chaque reinitialisation du microphone.
}

size_t AudioRingBuffer::write(const float* src, size_t n, AudioCaptureStats* stats) {
  if (src == nullptr || n == 0) return 0;

  size_t dropped = 0;

  // Ecriture plus grande que l'anneau entier : seuls les DERNIERS echantillons
  // ont un interet, le reste est deja perime avant meme d'etre stocke.
  if (n > MIC_RING_CAPACITY) {
    dropped = n - MIC_RING_CAPACITY;
    src += dropped;
    n = MIC_RING_CAPACITY;
  }

  // Place insuffisante : on abandonne les PLUS ANCIENS echantillons. Pour une
  // analyse temps reel, du son ancien n'a plus de valeur ; le son present en a.
  if (n > freeSpace()) {
    const size_t toDrop = n - freeSpace();
    _readIdx = (_readIdx + toDrop) & kMask;
    _count -= toDrop;
    dropped += toDrop;
  }

  size_t writeIdx = (_readIdx + _count) & kMask;
  const size_t firstChunk = (writeIdx + n <= MIC_RING_CAPACITY) ? n : (MIC_RING_CAPACITY - writeIdx);
  memcpy(&_buf[writeIdx], src, firstChunk * sizeof(float));
  if (firstChunk < n) {
    memcpy(&_buf[0], src + firstChunk, (n - firstChunk) * sizeof(float));
  }
  _count += n;

  if (stats) {
    stats->samplesReceived += (uint32_t)n;
    if (dropped > 0) {
      stats->bufferOverruns++;
      stats->droppedSamples += (uint32_t)dropped;
    }
  }
  return dropped;
}

bool AudioRingBuffer::readFrame(float* out, size_t frameSize, size_t hop,
                                AudioCaptureStats* stats) {
  if (out == nullptr || frameSize == 0 || frameSize > MIC_RING_CAPACITY) return false;
  if (hop == 0) hop = frameSize;
  if (hop > frameSize) hop = frameSize;

  // Une frame partielle n'est JAMAIS produite : c'est tout le point de cette
  // classe (defaut A0-1).
  if (_count < frameSize) {
    if (stats) stats->bufferUnderruns++;
    return false;
  }

  const size_t firstChunk =
      (_readIdx + frameSize <= MIC_RING_CAPACITY) ? frameSize : (MIC_RING_CAPACITY - _readIdx);
  memcpy(out, &_buf[_readIdx], firstChunk * sizeof(float));
  if (firstChunk < frameSize) {
    memcpy(out + firstChunk, &_buf[0], (frameSize - firstChunk) * sizeof(float));
  }

  // Avancer de `hop` et non de `frameSize` : c'est ce qui cree le recouvrement.
  // Les `frameSize - hop` derniers echantillons restent disponibles pour la
  // frame suivante.
  _readIdx = (_readIdx + hop) & kMask;
  _count -= hop;

  if (stats) stats->framesProduced++;
  return true;
}

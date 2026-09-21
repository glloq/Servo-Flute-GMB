/***********************************************************************************************
 * VibratoMath - Oscillateur sinusoidal du vibrato (CC1 Modulation)
 *
 * La table SIN_LUT est SIGNEE (int8_t, -127..+127) : elle contient une periode
 * complete. L'ancien code la lisait avec pgm_read_byte(), qui retourne un octet
 * NON signe : sur ESP32 la moitie negative de la sinusoide (indices 128-255)
 * remontait donc entre +129 et +255 au lieu de -1 a -127. Le vibrato etait
 * unipolaire et deux fois trop ample vers le haut - le souffle n'oscillait jamais
 * sous la valeur nominale. La lecture se fait desormais en int8_t.
 *
 * Isole dans son propre en-tete pour etre testable sur hote (tests natifs).
 ***********************************************************************************************/
#ifndef VIBRATO_MATH_H
#define VIBRATO_MATH_H

#include <Arduino.h>
#include "settings.h"

namespace VibratoMath {

// Table sinus signee, une periode sur SIN_LUT_SIZE entrees.
extern const int8_t SIN_LUT[SIN_LUT_SIZE];

// Valeur normalisee [-1.0, +1.0] pour un index de phase 0-255.
float sinLutAt(uint8_t index);

// Sinusoide rapide : retourne une valeur dans [-1.0, +1.0].
// Retourne 0 si la frequence est invalide (<= 0, non finie, ou trop haute pour
// que la periode entiere en millisecondes soit non nulle).
float fastSin(unsigned long timeMs, float frequency);

}  // namespace VibratoMath

#endif

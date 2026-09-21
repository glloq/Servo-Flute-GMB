/***********************************************************************************************
 * Sha256 - Implementation autonome de SHA-256 (FIPS 180-4)
 *
 * Volontairement independante de mbedTLS : le nom des fonctions mbedTLS change
 * entre ESP-IDF 4.x (mbedtls_sha256_starts_ret) et 5.x (mbedtls_sha256_starts),
 * et le firmware doit compiler tel quel sur les deux plateformes de la CI.
 * ~100 lignes, aucune allocation, et testable sur hote (vecteurs FIPS connus).
 *
 * Usage prevu : derivation salee et iteree du mot de passe administrateur de
 * l'interface web (DeviceSecrets). Ce n'est pas un KDF a cout memoire ; c'est un
 * durcissement raisonnable pour un secret qui ne quitte jamais la NVS.
 ***********************************************************************************************/
#ifndef SHA256_H
#define SHA256_H

#include <stdint.h>
#include <stddef.h>

#define SHA256_DIGEST_SIZE 32

class Sha256 {
public:
  Sha256();
  void reset();
  void update(const uint8_t* data, size_t len);
  // Ecrit SHA256_DIGEST_SIZE octets dans `digest`. L'etat est ensuite reinitialise.
  void finish(uint8_t* digest);

  // Raccourci une passe.
  static void hash(const uint8_t* data, size_t len, uint8_t* digest);

private:
  uint32_t _state[8];
  uint64_t _bitCount;
  uint8_t _buffer[64];
  size_t _bufferLen;
  void processBlock(const uint8_t* block);
};

#endif

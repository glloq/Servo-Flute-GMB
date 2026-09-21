#include "Sha256.h"

#include <string.h>

namespace {

const uint32_t kK[64] = {
  0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL, 0x3956c25bUL, 0x59f111f1UL,
  0x923f82a4UL, 0xab1c5ed5UL, 0xd807aa98UL, 0x12835b01UL, 0x243185beUL, 0x550c7dc3UL,
  0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL, 0xc19bf174UL, 0xe49b69c1UL, 0xefbe4786UL,
  0x0fc19dc6UL, 0x240ca1ccUL, 0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL,
  0x983e5152UL, 0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL, 0xc6e00bf3UL, 0xd5a79147UL,
  0x06ca6351UL, 0x14292967UL, 0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL, 0x53380d13UL,
  0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL, 0xa2bfe8a1UL, 0xa81a664bUL,
  0xc24b8b70UL, 0xc76c51a3UL, 0xd192e819UL, 0xd6990624UL, 0xf40e3585UL, 0x106aa070UL,
  0x19a4c116UL, 0x1e376c08UL, 0x2748774cUL, 0x34b0bcb5UL, 0x391c0cb3UL, 0x4ed8aa4aUL,
  0x5b9cca4fUL, 0x682e6ff3UL, 0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL,
  0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL
};

inline uint32_t rotr(uint32_t x, uint8_t n) { return (x >> n) | (x << (32 - n)); }

}  // namespace

Sha256::Sha256() { reset(); }

void Sha256::reset() {
  _state[0] = 0x6a09e667UL; _state[1] = 0xbb67ae85UL;
  _state[2] = 0x3c6ef372UL; _state[3] = 0xa54ff53aUL;
  _state[4] = 0x510e527fUL; _state[5] = 0x9b05688cUL;
  _state[6] = 0x1f83d9abUL; _state[7] = 0x5be0cd19UL;
  _bitCount = 0;
  _bufferLen = 0;
}

void Sha256::processBlock(const uint8_t* block) {
  uint32_t w[64];
  for (uint8_t i = 0; i < 16; i++) {
    w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
           ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
  }
  for (uint8_t i = 16; i < 64; i++) {
    uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  uint32_t a = _state[0], b = _state[1], c = _state[2], d = _state[3];
  uint32_t e = _state[4], f = _state[5], g = _state[6], h = _state[7];

  for (uint8_t i = 0; i < 64; i++) {
    uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    uint32_t ch = (e & f) ^ ((~e) & g);
    uint32_t temp1 = h + s1 + ch + kK[i] + w[i];
    uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t temp2 = s0 + maj;
    h = g; g = f; f = e; e = d + temp1;
    d = c; c = b; b = a; a = temp1 + temp2;
  }

  _state[0] += a; _state[1] += b; _state[2] += c; _state[3] += d;
  _state[4] += e; _state[5] += f; _state[6] += g; _state[7] += h;
}

void Sha256::update(const uint8_t* data, size_t len) {
  _bitCount += (uint64_t)len * 8;
  while (len > 0) {
    size_t take = 64 - _bufferLen;
    if (take > len) take = len;
    memcpy(_buffer + _bufferLen, data, take);
    _bufferLen += take;
    data += take;
    len -= take;
    if (_bufferLen == 64) {
      processBlock(_buffer);
      _bufferLen = 0;
    }
  }
}

void Sha256::finish(uint8_t* digest) {
  uint64_t bits = _bitCount;
  uint8_t pad = 0x80;
  update(&pad, 1);
  _bitCount = bits;   // le remplissage ne compte pas dans la longueur
  uint8_t zero = 0x00;
  while (_bufferLen != 56) {
    update(&zero, 1);
    _bitCount = bits;
  }
  uint8_t lengthBytes[8];
  for (int i = 7; i >= 0; i--) {
    lengthBytes[7 - i] = (uint8_t)((bits >> (i * 8)) & 0xFF);
  }
  memcpy(_buffer + _bufferLen, lengthBytes, 8);
  processBlock(_buffer);
  _bufferLen = 0;

  for (uint8_t i = 0; i < 8; i++) {
    digest[i * 4]     = (uint8_t)((_state[i] >> 24) & 0xFF);
    digest[i * 4 + 1] = (uint8_t)((_state[i] >> 16) & 0xFF);
    digest[i * 4 + 2] = (uint8_t)((_state[i] >> 8) & 0xFF);
    digest[i * 4 + 3] = (uint8_t)(_state[i] & 0xFF);
  }
  reset();
}

void Sha256::hash(const uint8_t* data, size_t len, uint8_t* digest) {
  Sha256 ctx;
  ctx.update(data, len);
  ctx.finish(digest);
}

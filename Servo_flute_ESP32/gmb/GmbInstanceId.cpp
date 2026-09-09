#include "GmbInstanceId.h"

namespace gmb {

void encode32Le7(uint32_t value, uint8_t out[5]) {
  out[0] = (uint8_t)(value & 0x7F);
  out[1] = (uint8_t)((value >> 7) & 0x7F);
  out[2] = (uint8_t)((value >> 14) & 0x7F);
  out[3] = (uint8_t)((value >> 21) & 0x7F);
  out[4] = (uint8_t)((value >> 28) & 0x0F);
}

uint32_t decode32Le7(const uint8_t in[5]) {
  return ((uint32_t)(in[0] & 0x7F)) |
         ((uint32_t)(in[1] & 0x7F) << 7) |
         ((uint32_t)(in[2] & 0x7F) << 14) |
         ((uint32_t)(in[3] & 0x7F) << 21) |
         ((uint32_t)(in[4] & 0x0F) << 28);
}

void encode21Le7(uint32_t value, uint8_t out[3]) {
  out[0] = (uint8_t)(value & 0x7F);
  out[1] = (uint8_t)((value >> 7) & 0x7F);
  out[2] = (uint8_t)((value >> 14) & 0x7F);
}

uint32_t decode21Le7(const uint8_t in[3]) {
  return ((uint32_t)(in[0] & 0x7F)) |
         ((uint32_t)(in[1] & 0x7F) << 7) |
         ((uint32_t)(in[2] & 0x7F) << 14);
}

uint32_t fnv1a32(const uint8_t* data, size_t len) {
  uint32_t hash = 2166136261u;
  if (data == 0) return hash;
  for (size_t i = 0; i < len; i++) {
    hash ^= (uint32_t)data[i];
    hash *= 16777619u;
  }
  return hash;
}

uint32_t instanceIdFromHardwareId(uint64_t hardwareId) {
  // Hash the 6 significant MAC bytes rather than truncating them: truncation to
  // 7-bit-per-byte would drop one bit out of every eight and bring two boards
  // whose MACs differ only in those bits onto the same identity.
  uint8_t bytes[6];
  for (int i = 0; i < 6; i++) bytes[i] = (uint8_t)((hardwareId >> (8 * i)) & 0xFF);
  uint32_t id = fnv1a32(bytes, sizeof(bytes));
  // 0 is reserved as "no identity"; keep the announced value non-zero.
  return id == 0 ? 1u : id;
}

}  // namespace gmb

/***********************************************************************************************
 * GmbInstanceId - 7-bit codecs and stable per-board instance identity (GMB v2).
 *
 * General-Midi-Boop identifies a physical instrument by a 32-bit `instance_id`
 * carried in the block-1 handshake (docs/GMB_PROTOCOL.md, GMB
 * docs/SYSEX_IDENTITY.md section 2). The identifier MUST differ between two boards
 * flashed with the same binary, and MUST survive a reboot, because it is the key
 * GMB uses to reattach a stored configuration to the right exemplar.
 *
 * This module is pure (no Arduino / ESP-IDF dependency) so the encoding is unit
 * tested on the host. The hardware source (ESP32 eFuse MAC) is read by GmbRuntime
 * and folded here.
 ***********************************************************************************************/
#ifndef GMB_INSTANCE_ID_H
#define GMB_INSTANCE_ID_H

#include <stddef.h>
#include <stdint.h>

namespace gmb {

// 32-bit value -> 5 little-endian 7-bit bytes. Bytes 0..3 carry 7 bits each and
// byte 4 carries bits 28-31 (a full nibble). This is the exact layout the GMB
// controller decodes in DeviceManager.parseGmbHandshake(); the 3-bit variant used
// by the legacy v1 codec would silently drop bit 31 of an instance id.
void encode32Le7(uint32_t value, uint8_t out[5]);
uint32_t decode32Le7(const uint8_t in[5]);

// 21-bit value -> 3 little-endian 7-bit bytes (descriptor_size field).
void encode21Le7(uint32_t value, uint8_t out[3]);
uint32_t decode21Le7(const uint8_t in[3]);

// Largest value the 21-bit descriptor_size field can carry.
static constexpr uint32_t kMaxDescriptorSize = 0x1FFFFFu;

// FNV-1a over `len` bytes. Deterministic and stable across builds/boots.
uint32_t fnv1a32(const uint8_t* data, size_t len);

// Fold a hardware unique id (the ESP32 48-bit eFuse MAC) into the announced
// 32-bit instance id. Never returns 0: {0,0,0,0,0} is explicitly non-conformant
// because two boards would then share an identity.
uint32_t instanceIdFromHardwareId(uint64_t hardwareId);

}  // namespace gmb

#endif

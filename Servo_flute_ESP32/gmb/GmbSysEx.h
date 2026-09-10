/***********************************************************************************************
 * GmbSysEx - General-Midi-Boop v2 SysEx wire codec.
 *
 *   F0 7D 00 <block> <direction> ... F7
 *   7D = experimental / educational manufacturer id, 00 = GMB id.
 *
 * Directions: 00 = request, 01 = response, 02 = notification.
 *
 * Blocks implemented (GMB docs/SYSEX_IDENTITY.md):
 *   0x01  handshake            request -> 24-byte response
 *   0x10  descriptor transfer  request(chunk) -> one JSON segment
 *   0x11  change notification  spontaneous, direction 0x02
 *
 * Transport independent: this codec only ever deals in complete MIDI byte buffers.
 ***********************************************************************************************/
#ifndef GMB_SYSEX_H
#define GMB_SYSEX_H

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "Capabilities.h"

namespace gmb {

enum SysExBlock : uint8_t {
  kBlockHandshake = 0x01,
  kBlockDescriptorTransfer = 0x10,
  kBlockChangeNotification = 0x11,
};

enum SysExDirection : uint8_t {
  kDirRequest = 0x00,
  kDirResponse = 0x01,
  kDirNotification = 0x02,
};

// Change flags of block 0x11 (GMB docs/SYSEX_IDENTITY.md section 4).
enum ChangeFlag : uint8_t {
  kIdentityChanged = 1 << 0,
  kInstrumentsChanged = 1 << 1,
  kTimingChanged = 1 << 2,
  kRestartRequired = 1 << 3,
};

// Handshake flags (section 2).
enum HandshakeFlag : uint8_t {
  kHttpDescriptorAvailable = 1 << 0,
  kChangeNotificationSupported = 1 << 1,
};

struct SysExRequest {
  bool valid = false;
  uint8_t block = 0;
  uint8_t direction = 0;
  bool hasChunkIndex = false;
  uint16_t chunkIndex = 0;
};

class GmbSysEx {
public:
  static constexpr uint8_t kStart = 0xF0;
  static constexpr uint8_t kEnd = 0xF7;
  static constexpr uint8_t kManufacturer = 0x7D;
  static constexpr uint8_t kGmbId = 0x00;
  static constexpr uint8_t kProtoVer = 0x02;

  // Longest GMB request we accept. Everything defined by the protocol is 6 or 8
  // bytes; a longer frame is not a GMB request and is dropped without being
  // buffered, which also caps what a flood of SysEx traffic can cost.
  static constexpr size_t kMaxRequestBytes = 16;
  // Descriptor payload per block 0x10 segment (section 3): 200 bytes keeps the
  // whole message at 210 bytes, under the BLE-MIDI reassembly limit.
  static constexpr size_t kDescriptorChunkPayload = 200;

  // 24-byte block-1 handshake response.
  static std::vector<uint8_t> encodeHandshake(const CapabilitySnapshot& snapshot,
                                              uint32_t descriptorSize,
                                              uint8_t flags);

  // One block 0x10 descriptor segment. Returns an empty vector when `index` is
  // outside the document (a malformed request is answered with silence rather
  // than with a frame that could be misassembled).
  static std::vector<uint8_t> encodeDescriptorChunk(const std::string& json, uint16_t index);

  // Number of segments a document of `jsonSize` bytes is transferred in.
  static uint16_t chunkCount(size_t jsonSize);

  // Block 0x11 spontaneous change notification (12 bytes).
  static std::vector<uint8_t> encodeChangeNotification(uint32_t revision, uint8_t changeFlags);

  // Header, trailer and 7-bit payload check. A frame that fails this is ignored.
  static bool isWellFormed(const uint8_t* data, size_t len);

  // Parse an incoming message. `valid` is false when it is not a GMB request.
  static SysExRequest parseRequest(const uint8_t* data, size_t len);
};

}  // namespace gmb

#endif

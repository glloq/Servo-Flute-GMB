#include "GmbSysEx.h"

#include "GmbInstanceId.h"

namespace gmb {

// ---------------------------------------------------------------------------
// Out-of-class definitions for the class constants.
//
// These are ODR-used (encodeHandshake() and friends pass them to
// std::vector::push_back(const uint8_t&), which binds a reference). Before
// C++17 that requires a namespace-scope definition; the ESP32 Arduino
// toolchain compiles this firmware with a pre-C++17 dialect, so without them
// the link fails with "undefined reference to gmb::GmbSysEx::kStart" and the
// others. From C++17 on the in-class initialiser is implicitly inline and
// these lines are redundant, hence the guard.
// ---------------------------------------------------------------------------
#if __cplusplus < 201703L
constexpr uint8_t GmbSysEx::kStart;
constexpr uint8_t GmbSysEx::kEnd;
constexpr uint8_t GmbSysEx::kManufacturer;
constexpr uint8_t GmbSysEx::kGmbId;
constexpr uint8_t GmbSysEx::kProtoVer;
constexpr size_t GmbSysEx::kMaxRequestBytes;
constexpr size_t GmbSysEx::kDescriptorChunkPayload;
#endif

namespace {

inline uint8_t b7(uint32_t v) { return (uint8_t)(v & 0x7F); }

void header(std::vector<uint8_t>& m, uint8_t block, uint8_t direction) {
  m.push_back(GmbSysEx::kStart);
  m.push_back(GmbSysEx::kManufacturer);
  m.push_back(GmbSysEx::kGmbId);
  m.push_back(block);
  m.push_back(direction);
}

void pushLe32(std::vector<uint8_t>& m, uint32_t value) {
  uint8_t enc[5];
  encode32Le7(value, enc);
  for (int i = 0; i < 5; i++) m.push_back(enc[i]);
}

void pushLe14(std::vector<uint8_t>& m, uint16_t value) {
  m.push_back(b7(value));
  m.push_back(b7((uint32_t)value >> 7));
}

}  // namespace

std::vector<uint8_t> GmbSysEx::encodeHandshake(const CapabilitySnapshot& snapshot,
                                               uint32_t descriptorSize,
                                               uint8_t flags) {
  std::vector<uint8_t> m;
  m.reserve(24);
  header(m, kBlockHandshake, kDirResponse);        // F0 7D 00 01 01
  m.push_back(kProtoVer);                          // proto_ver = 0x02
  pushLe32(m, snapshot.identity.instanceId);       // instance_id[5]
  for (int i = 0; i < 3; i++) m.push_back(b7(snapshot.identity.firmware[i]));
  if (descriptorSize > kMaxDescriptorSize) descriptorSize = kMaxDescriptorSize;
  uint8_t sizeEnc[3];
  encode21Le7(descriptorSize, sizeEnc);            // descriptor_size[3]
  for (int i = 0; i < 3; i++) m.push_back(sizeEnc[i]);
  pushLe32(m, snapshot.revision);                  // revision[5]
  m.push_back(b7(flags));                          // flags
  m.push_back(kEnd);
  return m;                                        // exactly 24 bytes
}

uint16_t GmbSysEx::chunkCount(size_t jsonSize) {
  size_t total = (jsonSize + kDescriptorChunkPayload - 1) / kDescriptorChunkPayload;
  if (total == 0) total = 1;   // always advertise at least one (possibly empty) segment
  if (total > 0x3FFF) total = 0x3FFF;  // 14-bit field
  return (uint16_t)total;
}

std::vector<uint8_t> GmbSysEx::encodeDescriptorChunk(const std::string& json, uint16_t index) {
  const uint16_t total = chunkCount(json.size());
  // Fail safely on an out-of-range segment: no frame at all, so a controller can
  // never reassemble a document from a segment that does not exist.
  if (index >= total) return std::vector<uint8_t>();

  std::vector<uint8_t> m;
  m.reserve(10 + kDescriptorChunkPayload);
  header(m, kBlockDescriptorTransfer, kDirResponse);  // F0 7D 00 10 01
  pushLe14(m, total);                                  // total_chunks[2]
  pushLe14(m, index);                                  // chunk_index[2]
  const size_t start = (size_t)index * kDescriptorChunkPayload;
  for (size_t i = start; i < json.size() && i < start + kDescriptorChunkPayload; i++) {
    // The descriptor is 7-bit ASCII by construction; mask anyway so no encoder bug
    // can ever put a status byte inside a SysEx payload.
    m.push_back(b7((uint8_t)json[i]));
  }
  m.push_back(kEnd);
  return m;
}

std::vector<uint8_t> GmbSysEx::encodeChangeNotification(uint32_t revision, uint8_t changeFlags) {
  std::vector<uint8_t> m;
  m.reserve(12);
  header(m, kBlockChangeNotification, kDirNotification);  // F0 7D 00 11 02
  pushLe32(m, revision);                                  // revision[5]
  m.push_back(b7(changeFlags));                           // change_flags
  m.push_back(kEnd);
  return m;                                               // exactly 12 bytes
}

bool GmbSysEx::isWellFormed(const uint8_t* data, size_t len) {
  if (data == 0 || len < 6) return false;
  if (data[0] != kStart || data[len - 1] != kEnd) return false;
  if (data[1] != kManufacturer || data[2] != kGmbId) return false;
  // Every byte between the two status bytes must be 7-bit.
  for (size_t i = 1; i < len - 1; i++) {
    if (data[i] & 0x80) return false;
  }
  return true;
}

SysExRequest GmbSysEx::parseRequest(const uint8_t* data, size_t len) {
  SysExRequest r;
  if (!isWellFormed(data, len)) return r;
  if (len > kMaxRequestBytes) return r;   // not a GMB request frame

  r.block = data[3];
  r.direction = data[4];
  // Only requests are answered; a response or a notification coming back in is
  // never echoed (that would let two instruments on one bus talk to each other).
  if (r.direction != kDirRequest) return r;

  if (r.block == kBlockHandshake) {
    if (len != 6) return r;              // F0 7D 00 01 00 F7
    r.valid = true;
    return r;
  }
  if (r.block == kBlockDescriptorTransfer) {
    if (len != 8) return r;              // F0 7D 00 10 00 <idx lo> <idx hi> F7
    r.hasChunkIndex = true;
    r.chunkIndex = (uint16_t)((data[5] & 0x7F) | ((uint16_t)(data[6] & 0x7F) << 7));
    r.valid = true;
    return r;
  }
  // Unknown block: ignored.
  return r;
}

}  // namespace gmb

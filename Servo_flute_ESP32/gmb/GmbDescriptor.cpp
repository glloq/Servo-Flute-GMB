#include "GmbDescriptor.h"

namespace gmb {

namespace {

void appendHex4(std::string& out, unsigned cp) {
  static const char* kHex = "0123456789abcdef";
  out += "\\u";
  out += kHex[(cp >> 12) & 0xF];
  out += kHex[(cp >> 8) & 0xF];
  out += kHex[(cp >> 4) & 0xF];
  out += kHex[cp & 0xF];
}

// JSON-escape a UTF-8 string into 7-bit ASCII. Quote / backslash / control
// characters are escaped and every non-ASCII code point becomes \uXXXX, so the
// whole document stays 7-bit safe for the SysEx transfer.
std::string esc(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 8);
  const size_t n = in.size();
  size_t i = 0;
  while (i < n) {
    const unsigned char c = (unsigned char)in[i];
    if (c == '"') { out += "\\\""; ++i; }
    else if (c == '\\') { out += "\\\\"; ++i; }
    else if (c == '\n') { out += "\\n"; ++i; }
    else if (c == '\r') { out += "\\r"; ++i; }
    else if (c == '\t') { out += "\\t"; ++i; }
    else if (c < 0x20 || c == 0x7F) { appendHex4(out, c); ++i; }
    else if (c < 0x80) { out += (char)c; ++i; }
    else {
      // Decode one UTF-8 sequence to a code point and emit it escaped.
      unsigned cp = 0xFFFD;
      size_t adv = 1;
      if ((c & 0xE0) == 0xC0 && i + 1 < n) {
        cp = ((c & 0x1Fu) << 6) | ((unsigned char)in[i + 1] & 0x3Fu);
        adv = 2;
      } else if ((c & 0xF0) == 0xE0 && i + 2 < n) {
        cp = ((c & 0x0Fu) << 12) | (((unsigned char)in[i + 1] & 0x3Fu) << 6) |
             ((unsigned char)in[i + 2] & 0x3Fu);
        adv = 3;
      } else if ((c & 0xF8) == 0xF0 && i + 3 < n) {
        cp = 0xFFFD;  // outside the BMP: replacement character
        adv = 4;
      }
      appendHex4(out, cp);
      i += adv;
    }
  }
  return out;
}

void appendByteArray(std::string& j, const std::vector<uint8_t>& v) {
  j += '[';
  for (size_t i = 0; i < v.size(); i++) {
    if (i) j += ',';
    j += std::to_string((int)v[i]);
  }
  j += ']';
}

void appendKeyString(std::string& j, const char* key, const std::string& value) {
  j += ",\"";
  j += key;
  j += "\":\"";
  j += esc(value);
  j += '"';
}

void appendKeyInt(std::string& j, const char* key, long value) {
  j += ",\"";
  j += key;
  j += "\":";
  j += std::to_string(value);
}

void appendKeyBool(std::string& j, const char* key, bool value) {
  j += ",\"";
  j += key;
  j += "\":";
  j += value ? "true" : "false";
}

}  // namespace

std::string GmbDescriptor::toJson(const CapabilitySnapshot& s) {
  const InstrumentCapabilities& inst = s.instrument;

  std::string j;
  j.reserve(1024);
  j += "{\"gmb_descriptor\":2";
  j += ",\"revision\":" + std::to_string((unsigned long)s.revision);
  j += ",\"device\":{\"name\":\"" + esc(s.identity.deviceName) + "\"";
  j += ",\"model\":\"" + esc(s.identity.model) + "\"}";

  j += ",\"instruments\":[{";
  j += "\"channel\":" + std::to_string((int)inst.channel);
  j += ",\"configured\":";
  j += inst.configured ? "true" : "false";

  if (!inst.configured) {
    // Section 5.1: the instrument exists but is not defined. Announce nothing
    // else, so GMB hands the user manual entry without overwriting a previous
    // configuration with fabricated fallback capabilities.
    j += "}]}";
    return j;
  }

  appendKeyString(j, "name", inst.name);
  appendKeyInt(j, "gm_program", inst.gmProgram);
  appendKeyString(j, "type", inst.type);
  appendKeyString(j, "subtype", inst.subtype);

  // notes: a contiguous set is announced as a range, anything else as the exact
  // discrete list. Disabled / invalid fingerings never reach this list.
  j += ",\"notes\":";
  if (inst.noteMode == kNoteRange) {
    j += "{\"mode\":\"range\",\"min\":" + std::to_string((int)inst.noteMin) +
         ",\"max\":" + std::to_string((int)inst.noteMax) + "}";
  } else {
    j += "{\"mode\":\"discrete\",\"list\":";
    appendByteArray(j, inst.notes);
    j += "}";
  }

  j += ",\"polyphony\":{\"max\":" + std::to_string((int)inst.polyphony) + "}";

  // timing: the two-phase model. Only measured / configured values are emitted;
  // an unknown field is absent, never 0.
  const TimingModel& t = inst.timing;
  if (t.hasPrepare || t.hasExciteLatency || t.hasMinNote || t.hasRearticulation) {
    j += ",\"timing\":{";
    bool first = true;
    if (t.hasPrepare) {
      j += "\"prepare\":{\"base_ms\":" + std::to_string((int)t.prepareBaseMs) +
           ",\"max_ms\":" + std::to_string((int)t.prepareMaxMs) + ",\"silent\":true}";
      first = false;
    }
    if (t.hasExciteLatency) {
      if (!first) j += ',';
      j += "\"excite\":{\"latency_ms\":" + std::to_string((int)t.exciteLatencyMs) + "}";
      first = false;
    }
    if (t.hasMinNote) {
      if (!first) j += ',';
      j += "\"min_note_ms\":" + std::to_string((int)t.minNoteMs);
      first = false;
    }
    if (t.hasRearticulation) {
      if (!first) j += ',';
      j += "\"rearticulation_ms\":" + std::to_string((int)t.rearticulationMs);
      first = false;
    }
    j += "}";
  }

  // expression: only what the firmware really implements and the configuration
  // really enables.
  const ExpressionModel& e = inst.expression;
  j += ",\"expression\":{\"cc\":";
  appendByteArray(j, e.cc);
  j += ",\"pitch_bend\":{\"supported\":";
  j += e.pitchBend ? "true" : "false";
  if (e.pitchBend) j += ",\"range_semitones\":" + std::to_string((int)e.pitchBendRangeSemitones);
  j += "}";
  j += ",\"channel_aftertouch\":";
  j += e.channelAftertouch ? "true" : "false";
  j += ",\"poly_aftertouch\":";
  j += e.polyAftertouch ? "true" : "false";
  j += ",\"velocity\":";
  j += e.velocity ? "true" : "false";
  j += "}";

  // physical: the wind-family block. Generic keys keep their generic meaning;
  // the flute-specific extras are ignored by any host that does not know them.
  const PhysicalModel& p = inst.physical;
  j += ",\"physical\":{\"family\":\"" + esc(p.family) + "\"";
  appendKeyString(j, "embouchure", p.embouchure);
  appendKeyInt(j, "finger_count", p.fingerCount);
  appendKeyInt(j, "thumb_hole_count", p.thumbHoleCount);
  appendKeyBool(j, "half_holes", p.halfHoles);
  appendKeyString(j, "air_source", p.airSource);
  appendKeyBool(j, "jet_angle_control", p.jetAngleControl);
  j += "}";

  j += "}]}";
  return j;
}

}  // namespace gmb

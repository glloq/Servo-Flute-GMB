#include "SerialMidiHandler.h"
#include "InstrumentManager.h"
#include "ConfigStorage.h"

// MIDI baudrate standard
#define MIDI_BAUD_RATE 31250

SerialMidiHandler::SerialMidiHandler()
  : _instrument(nullptr), _serial(nullptr),
    _enabled(false), _running(false), _rxPin(0),
    _status(0), _dataIndex(0), _expectedLen(0) {
  _data[0] = 0;
  _data[1] = 0;
}

void SerialMidiHandler::begin(InstrumentManager* instrument) {
  _instrument = instrument;
  _enabled = cfg.serialMidiEnabled;
  _rxPin = cfg.serialMidiRxPin;

  if (!_enabled) {
    if (DEBUG) {
      Serial.println("DEBUG: SerialMidiHandler - Desactive dans la config");
    }
    return;
  }

  // Use UART2 for MIDI input (UART0 = USB debug, UART1 = flash)
  _serial = &Serial2;

  // Begin with RX only (TX pin = -1, unused)
  _serial->begin(MIDI_BAUD_RATE, SERIAL_8N1, _rxPin, -1);

  _running = true;

  // Reset parser state
  _status = 0;
  _dataIndex = 0;
  _expectedLen = 0;

  if (DEBUG) {
    Serial.print("DEBUG: SerialMidiHandler - MIDI Serial actif sur GPIO");
    Serial.println(_rxPin);
  }
}

void SerialMidiHandler::stop() {
  if (_serial && _running) {
    _serial->end();
    _running = false;
    if (DEBUG) {
      Serial.println("DEBUG: SerialMidiHandler - Arrete");
    }
  }
}

void SerialMidiHandler::update() {
  if (!_running || !_serial) return;

  // Read all available MIDI bytes
  while (_serial->available()) {
    uint8_t b = _serial->read();
    parseByte(b);
  }
}

void SerialMidiHandler::parseByte(uint8_t byte) {
  // Real-time messages (0xF8-0xFF) - ignore, single byte
  if (byte >= 0xF8) {
    return;
  }

  // System Common messages (0xF0-0xF7) - reset running status, ignore
  if (byte >= 0xF0) {
    _status = 0;
    _dataIndex = 0;
    return;
  }

  // Status byte (0x80-0xEF)
  if (byte & 0x80) {
    _status = byte;
    _dataIndex = 0;

    // Determine expected data bytes
    uint8_t type = byte & 0xF0;
    switch (type) {
      case 0x80:  // Note Off
      case 0x90:  // Note On
      case 0xA0:  // Aftertouch poly
      case 0xB0:  // Control Change
      case 0xE0:  // Pitch Bend
        _expectedLen = 2;
        break;
      case 0xC0:  // Program Change
      case 0xD0:  // Channel Pressure
        _expectedLen = 1;
        break;
      default:
        _expectedLen = 0;
        break;
    }
    return;
  }

  // Data byte (0x00-0x7F) - requires valid running status
  if (_status == 0) return;

  _data[_dataIndex++] = byte;

  if (_dataIndex >= _expectedLen) {
    processMessage(_status, _data[0], (_expectedLen > 1) ? _data[1] : 0);
    _dataIndex = 0;  // Reset for running status (next message reuses same status)
  }
}

void SerialMidiHandler::processMessage(uint8_t status, uint8_t data1, uint8_t data2) {
  uint8_t type = status & 0xF0;
  uint8_t channel = (status & 0x0F) + 1;  // MIDI channels 1-16

  if (!isChannelAccepted(channel)) return;

  switch (type) {
    case 0x90:  // Note On
      if (data2 > 0) {
        if (_instrument) _instrument->noteOn(data1, data2);
        if (DEBUG) {
          Serial.print("DEBUG: SerialMIDI NoteOn n=");
          Serial.print(data1);
          Serial.print(" v=");
          Serial.println(data2);
        }
      } else {
        // Note On with velocity 0 = Note Off
        if (_instrument) _instrument->noteOff(data1);
      }
      break;

    case 0x80:  // Note Off
      if (_instrument) _instrument->noteOff(data1);
      break;

    case 0xB0:  // Control Change
      if (_instrument) _instrument->handleControlChange(data1, data2);
      break;

    default:
      break;
  }
}

bool SerialMidiHandler::isChannelAccepted(uint8_t channel) {
  if (cfg.midiChannel == 0) return true;  // Omni mode
  return (channel == cfg.midiChannel);
}

/***********************************************************************************************
 * SerialMidiHandler - MIDI input via serial (hardware UART)
 *
 * Receives standard MIDI messages (31250 baud) on a configurable ESP32 GPIO pin
 * using HardwareSerial (UART2). This allows connecting a classic MIDI DIN input
 * circuit (optocoupler + DIN5 connector) to the ESP32.
 *
 * Supported messages:
 * - Note On (0x90)
 * - Note Off (0x80)
 * - Control Change (0xB0)
 *
 * Configuration (via web settings):
 * - Enable/disable
 * - RX pin (GPIO number)
 *
 * The serial MIDI input works independently and in parallel with BLE or WiFi MIDI.
 *
 * Dependances : HardwareSerial (built-in ESP32)
 ***********************************************************************************************/
#ifndef SERIAL_MIDI_HANDLER_H
#define SERIAL_MIDI_HANDLER_H

#include <Arduino.h>
#include "settings.h"

// Forward declaration
class InstrumentManager;

class SerialMidiHandler {
public:
  SerialMidiHandler();

  // Initialise le port serie MIDI (UART2, 31250 baud)
  void begin(InstrumentManager* instrument);

  // Arrete le port serie MIDI
  void stop();

  // Lit et traite les messages MIDI entrants (appeler dans loop)
  void update();

  // Etat
  bool isEnabled() const { return _enabled; }
  bool isRunning() const { return _running; }

private:
  InstrumentManager* _instrument;
  HardwareSerial* _serial;
  bool _enabled;
  bool _running;
  uint8_t _rxPin;

  // MIDI parser state
  uint8_t _status;        // Running status byte
  uint8_t _data[2];       // Data bytes buffer
  uint8_t _dataIndex;     // Current data byte index
  uint8_t _expectedLen;   // Expected data bytes for current status

  // Parse un octet MIDI entrant
  void parseByte(uint8_t byte);

  // Traite un message MIDI complet
  void processMessage(uint8_t status, uint8_t data1, uint8_t data2);

  // Verifie si le canal MIDI est accepte
  bool isChannelAccepted(uint8_t channel);
};

#endif

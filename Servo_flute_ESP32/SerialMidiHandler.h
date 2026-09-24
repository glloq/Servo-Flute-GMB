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

// Plafond d'octets lus par passe de update(). La constante vit ICI, dans le
// module qui borne, et non dans settings.h qui est partage.
//
// 64 octets, pour deux raisons qui convergent :
//  - c'est environ 21 messages de 3 octets, soit un peu moins que la capacite
//    de l'anneau de commandes (COMMAND_QUEUE_SIZE = 24). En lire davantage ne
//    ferait que remplir un anneau que loop() vide par tranches de 6 ;
//  - a 31250 bauds c'est environ 20 ms de trafic MIDI nominal, bien plus que
//    ce qui s'accumule entre deux passes de loop(). Le trafic normal ne
//    rencontre donc JAMAIS cette borne : elle ne se declenche que face a une
//    source qui emet plus vite que le MIDI, c'est-a-dire une anomalie.
#define MIDI_SERIAL_MAX_BYTES_PER_UPDATE 64

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

  // Surveillance de perte de lien (Active Sensing 0xFE).
  // Une source qui emet de l'Active Sensing s'engage a envoyer au moins un octet
  // toutes les ~300 ms ; son silence prolonge signale un cable debranche. On
  // declenche alors un panic pour ne pas laisser une note DIN bloquee.
  bool _activeSensing;    // au moins un 0xFE recu
  bool _linkLost;         // panic deja emis pour ce silence (evite la repetition)
  unsigned long _lastByteMs;  // horodatage du dernier octet recu

  // Parse un octet MIDI entrant
  void parseByte(uint8_t byte);

  // Coupe le son si l'Active Sensing s'est tu au-dela du timeout.
  void checkActiveSensingTimeout();

  // Traite un message MIDI complet
  void processMessage(uint8_t status, uint8_t data1, uint8_t data2);

  // Verifie si le canal MIDI est accepte
  bool isChannelAccepted(uint8_t channel);
};

#endif

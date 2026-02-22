/***********************************************************************************************
 * MidiFilePlayer - Parseur et lecteur de fichiers MIDI standard (SMF)
 *
 * Supporte :
 * - SMF Type 0 (piste unique) et Type 1 (multi-pistes, merge)
 * - Note On / Note Off
 * - Control Change
 * - Meta events : Tempo, End of Track
 * - Variable Length Quantity (VLQ)
 *
 * Fonctionnement :
 * 1. Upload du fichier .mid sur LittleFS
 * 2. Parsing : extraction des evenements en memoire (max MIDI_FILE_MAX_EVENTS)
 * 3. Playback non-bloquant via update() dans loop()
 * 4. Les notes sont envoyees a InstrumentManager (comme BLE/WiFi MIDI)
 *
 * Dependances : LittleFS
 ***********************************************************************************************/
#ifndef MIDI_FILE_PLAYER_H
#define MIDI_FILE_PLAYER_H

#include <Arduino.h>
#include <LittleFS.h>
#include "settings.h"

// Forward declaration
class InstrumentManager;

// Evenement MIDI parse (compact : 8 octets)
struct MidiFileEvent {
  uint32_t absoluteTimeMs;  // Temps absolu en ms depuis le debut
  uint8_t type;             // 0x90=NoteOn, 0x80=NoteOff, 0xB0=CC
  uint8_t channel;          // Canal MIDI (0-15)
  uint8_t data1;            // Note ou numero CC
  uint8_t data2;            // Velocity ou valeur CC
};

enum PlayerState {
  PLAYER_STOPPED,
  PLAYER_PLAYING,
  PLAYER_PAUSED
};

class MidiFilePlayer {
public:
  MidiFilePlayer();
  ~MidiFilePlayer();

  void begin(InstrumentManager* instrument);

  // Charger et parser un fichier MIDI depuis LittleFS
  bool loadFile(const char* path);

  // Controles de lecture
  void play();
  void pause();
  void stop();

  // Appeler dans loop() pour le playback non-bloquant
  void update();

  // Etat
  PlayerState getState() const;
  uint16_t getEventCount() const;
  uint32_t getDurationMs() const;
  uint32_t getPositionMs() const;
  float getProgressPercent() const;
  String getFileName() const;
  bool isFileLoaded() const;

  // Filtre canal: 0-15 = canal specifique, 255 = tous (defaut)
  void setChannelFilter(uint8_t channel);
  uint8_t getChannelFilter() const;
  // Bitmask des canaux presents dans le fichier (bit 0 = ch 0, etc.)
  uint16_t getActiveChannels() const;

private:
  InstrumentManager* _instrument;
  PlayerState _state;

  // Evenements parses
  MidiFileEvent* _events;
  uint16_t _eventCount;
  uint16_t _currentEvent;

  // Timing
  uint32_t _durationMs;
  unsigned long _playbackStartMs;
  uint32_t _pausePositionMs;

  // Metadonnees
  String _fileName;
  bool _fileLoaded;
  uint8_t _channelFilter;     // 255 = tous canaux
  uint16_t _activeChannels;   // bitmask canaux presents

  // Parsing MIDI
  bool parseFile(File& file);
  bool parseMThd(File& file, uint16_t& format, uint16_t& numTracks, uint16_t& division);
  bool parseMTrk(File& file, uint32_t trackLength, uint16_t division, uint32_t baseTempo);
  uint32_t readVLQ(File& file, uint32_t& bytesRead);
  uint16_t readU16(File& file);
  uint32_t readU32(File& file);

  // Inserer un evenement trie par temps
  void insertEvent(const MidiFileEvent& evt);

  // Convertir ticks en ms
  uint32_t ticksToMs(uint32_t ticks, uint32_t tempo, uint16_t division);

  // Tri des evenements par temps (pour merge multi-pistes)
  void sortEvents();
};

#endif

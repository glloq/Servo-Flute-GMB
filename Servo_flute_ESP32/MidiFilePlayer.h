/***********************************************************************************************
 * MidiFilePlayer - Parseur et lecteur de fichiers MIDI standard (SMF)
 *
 * Supporte :
 * - SMF Type 0 (piste unique) et Type 1 (multi-pistes, merge avec carte de
 *   tempo GLOBALE : les changements de tempo de la piste 0 s'appliquent bien
 *   aux notes des autres pistes)
 * - Note On / Note Off
 * - Control Change
 * - Meta events : Tempo, End of Track
 * - Variable Length Quantity (VLQ)
 *
 * Non supporte (rejete explicitement via getLoadError()) :
 * - SMF Type 2 (sequences independantes)
 * - Divisions temporelles SMPTE
 * - Fichiers depassant MIDI_FILE_MAX_EVENTS evenements (refuses, jamais tronques)
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
#include "MidiTempoMap.h"

// Forward declaration
class InstrumentManager;

// Evenement MIDI parse (compact : 8 octets).
// Pendant le parsing, absoluteTimeMs contient d'abord le TICK absolu ; il est
// converti en millisecondes une fois la carte de tempo globale finalisee.
struct MidiFileEvent {
  uint32_t absoluteTimeMs;  // Temps absolu en ms (tick pendant le parsing)
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

// Motif d'echec de loadFile(). Expose via getLoadError() pour que l'API web
// puisse renvoyer une raison precise plutot qu'un vague "Invalid MIDI format".
enum MidiLoadError {
  MIDI_LOAD_OK = 0,
  MIDI_LOAD_ERR_OPEN,             // fichier introuvable / illisible
  MIDI_LOAD_ERR_HEADER,          // MThd absent ou tronque
  MIDI_LOAD_ERR_SMPTE,           // division SMPTE non supportee
  MIDI_LOAD_ERR_DIVISION,        // ticks/noire nul (fichier corrompu)
  MIDI_LOAD_ERR_FORMAT2,         // SMF Type 2 (sequences independantes) non supporte
  MIDI_LOAD_ERR_EVENT_LIMIT,     // trop d'evenements : fichier tronque -> refuse
  MIDI_LOAD_ERR_NO_EVENTS        // aucun evenement jouable
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

  // Cause du dernier echec de loadFile() (MIDI_LOAD_OK si le dernier chargement
  // a reussi). getLoadErrorCode() renvoie un code court stable pour l'API/JSON.
  MidiLoadError getLoadError() const;
  const char* getLoadErrorCode() const;

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

  // Etat du parsing
  MidiLoadError _loadError;   // cause du dernier echec (ou MIDI_LOAD_OK)
  bool _truncated;            // limite MIDI_FILE_MAX_EVENTS atteinte
  MidiTempoMap _tempoMap;     // carte globale tick -> tempo (toutes pistes)

  // Parsing MIDI
  bool parseFile(File& file);
  bool parseMThd(File& file, uint16_t& format, uint16_t& numTracks, uint16_t& division);
  // Passe 1 : lit une piste, stocke les evenements en TICKS absolus et alimente
  // la carte de tempo globale (les changements de tempo de toutes les pistes).
  bool parseMTrk(File& file, uint32_t trackLength);
  uint32_t readVLQ(File& file, uint32_t& bytesRead);
  uint16_t readU16(File& file);
  uint32_t readU32(File& file);

  // Inserer un evenement (stocke en ticks pendant la passe 1). Signale la
  // troncature si la capacite est atteinte.
  void insertEvent(const MidiFileEvent& evt);

  // Passe 2 : trie par tick puis convertit chaque tick en ms via _tempoMap.
  void convertTicksToMs();

  // Tri des evenements par temps/tick (pour merge multi-pistes)
  void sortEvents();
};

#endif

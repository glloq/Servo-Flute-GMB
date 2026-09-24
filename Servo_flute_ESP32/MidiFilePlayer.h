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

/*------------------------------------------------------------------------------
 * BORNE DE TRAVAIL PAR PASSE D'update()
 *
 * update() drainait TOUS les evenements echus en un seul tour de loop(). Comme
 * un fichier peut en porter MIDI_FILE_MAX_EVENTS = 2000, une rafale au MEME
 * horodatage - ou un lecteur qui rattrape un gros retard de millis() apres une
 * ecriture LittleFS, une reconnexion Wi-Fi ou un upload - pouvait emettre
 * jusqu'a 2000 ordres d'affilee vers l'instrument. Pendant ce temps rien
 * d'autre de loop() ne tourne : ni regulation de pression, ni ventilateur, ni
 * audio, ni WebSocket, ni chien de garde, ni securite actionneurs.
 *
 * POURQUOI 16, ET PAS UN AUTRE NOMBRE
 * -----------------------------------
 * 1. C'est la capacite de la file qui recoit ces ordres. Le lecteur ne passe
 *    PAS par l'anneau de commandes (INSTRUMENT_MAX_COMMANDS_PER_UPDATE) : il
 *    s'execute deja sur la tache loop() et appelle directement noteOn(),
 *    noteOff() et handleControlChange(). Ses evenements atterrissent donc dans
 *    l'EventQueue de l'instrument, qui tient EVENT_QUEUE_SIZE = 16 evenements.
 *    Le 17e ordre d'une meme passe ne peut rien produire de bon : un Note On de
 *    plus est REFUSE (note perdue), un Note Off de plus est enfile de FORCE et
 *    evince le plus ancien. Passe 16, le travail supplementaire ne fait que
 *    detruire des notes deja emises dans la meme passe.
 * 2. Aucun morceau reel ne s'en approche sur cet instrument. Le sequenceur est
 *    MONOPHONIQUE : a un instant donne, un fichier ecrit pour lui porte le
 *    Note Off de la note qui finit et le Note On de celle qui commence, soit
 *    2 evenements. Meme un fichier polyphonique joue tel quel (un accord de six
 *    voix relache et reattaque au meme tick = 12 evenements, plus quelques CC)
 *    reste sous la borne : dans le cas courant, la borne ne se voit pas.
 * 3. Le firmware a deja arbitre ce rapport ailleurs, dans le meme sens :
 *    INSTRUMENT_MAX_NOTE_OFFS_PER_UPDATE vaut 8 = EVENT_QUEUE_SIZE / 2,
 *    precisement pour qu'une rafale de relachements ne se mange pas elle-meme.
 *    16 est la meme grandeur, prise a la capacite entiere parce qu'ici le
 *    lecteur est la seule source qui remplit la file pendant sa passe.
 *
 * CE QU'ELLE COUTE. Le pire cas theorique - 2000 evenements tous echus -
 * demande 125 passes de loop() au lieu d'une. loop() ne porte aucune
 * temporisation (voir Servo_flute_ESP32.ino) : une passe vaut le temps du
 * travail lui-meme, pas une periode fixe. Et ce pire cas n'etait de toute
 * facon pas jouable : la file de l'instrument n'en absorbe que 16.
 *
 * CE QU'ELLE NE RETARDE PAS. stop(), pause() et allSoundOff() ne traversent pas
 * cette boucle : ils agissent immediatement, quel que soit le retard accumule.
 * Les Channel Mode Messages (CC 120-127) presents DANS le fichier ne sont pas
 * soumis a la borne non plus - voir update().
 *
 * Cette constante vit ICI et pas dans settings.h : elle decrit le rythme
 * interne du lecteur, pas une option d'instrument.
 *----------------------------------------------------------------------------*/
static const uint8_t MIDI_PLAYER_MAX_EVENTS_PER_UPDATE = 16;

class MidiFilePlayer {
public:
  MidiFilePlayer();
  ~MidiFilePlayer();

  // Rend false si le tableau d'evenements n'a pas pu etre alloue. Le lecteur
  // reste alors utilisable mais INERTE : aucun fichier ne se chargera. Le
  // savoir permet de le DIRE, au lieu de laisser l'utilisateur constater que
  // ses fichiers MIDI ne se chargent plus sans jamais apprendre pourquoi.
  bool begin(InstrumentManager* instrument);

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

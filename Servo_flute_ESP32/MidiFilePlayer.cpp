#include "MidiFilePlayer.h"
#include "InstrumentManager.h"

MidiFilePlayer::MidiFilePlayer()
  : _instrument(nullptr), _state(PLAYER_STOPPED),
    _events(nullptr), _eventCount(0), _currentEvent(0),
    _durationMs(0), _playbackStartMs(0), _pausePositionMs(0),
    _fileLoaded(false), _channelFilter(255), _activeChannels(0),
    _loadError(MIDI_LOAD_OK), _truncated(false) {
}

MidiFilePlayer::~MidiFilePlayer() {
  if (_events != nullptr) {
    delete[] _events;
  }
}

void MidiFilePlayer::begin(InstrumentManager* instrument) {
  _instrument = instrument;
  // Pre-allouer le tableau d'evenements
  _events = new MidiFileEvent[MIDI_FILE_MAX_EVENTS];
  if (DEBUG) {
    Serial.println("DEBUG: MidiFilePlayer - Init OK");
  }
}

bool MidiFilePlayer::loadFile(const char* path) {
  if (_events == nullptr) return false;

  // Arreter toute lecture en cours
  stop();
  _eventCount = 0;
  _fileLoaded = false;
  _loadError = MIDI_LOAD_OK;
  _truncated = false;

  File file = LittleFS.open(path, "r");
  if (!file) {
    _loadError = MIDI_LOAD_ERR_OPEN;
    if (DEBUG) {
      Serial.println("ERREUR: MidiFilePlayer - Impossible d'ouvrir le fichier");
    }
    return false;
  }

  // Extraire le nom du fichier
  _fileName = String(path);
  int lastSlash = _fileName.lastIndexOf('/');
  if (lastSlash >= 0) {
    _fileName = _fileName.substring(lastSlash + 1);
  }

  if (DEBUG) {
    Serial.print("DEBUG: MidiFilePlayer - Chargement: ");
    Serial.print(_fileName);
    Serial.print(" (");
    Serial.print(file.size());
    Serial.println(" octets)");
  }

  bool success = parseFile(file);
  file.close();

  // Refuser explicitement un fichier tronque plutot que de le declarer valide
  // en n'en jouant qu'une partie (audit P0 : troncature silencieuse).
  if (_truncated || _tempoMap.overflowed()) {
    _eventCount = 0;
    if (_loadError == MIDI_LOAD_OK) _loadError = MIDI_LOAD_ERR_EVENT_LIMIT;
    if (DEBUG) {
      Serial.println("ERREUR: MidiFilePlayer - Fichier trop dense (limite d'evenements atteinte), refuse");
    }
    return false;
  }

  if (!success) {
    // _loadError a deja ete positionne par parseMThd/parseFile
    if (DEBUG) {
      Serial.println("ERREUR: MidiFilePlayer - Echec parsing");
    }
    return false;
  }

  if (_eventCount == 0) {
    _loadError = MIDI_LOAD_ERR_NO_EVENTS;
    if (DEBUG) {
      Serial.println("ERREUR: MidiFilePlayer - Aucun evenement jouable");
    }
    return false;
  }

  // Passe 2 : carte de tempo globale finalisee, tri par tick, conversion en ms.
  convertTicksToMs();

  _durationMs = _events[_eventCount - 1].absoluteTimeMs;
  _fileLoaded = true;
  // Scanner les canaux presents
  _activeChannels = 0;
  for (uint16_t i = 0; i < _eventCount; i++) {
    _activeChannels |= (1 << _events[i].channel);
  }

  if (DEBUG) {
    Serial.print("DEBUG: MidiFilePlayer - Parse OK: ");
    Serial.print(_eventCount);
    Serial.print(" evenements, duree: ");
    Serial.print(_durationMs / 1000);
    Serial.println("s");
  }

  return _fileLoaded;
}

void MidiFilePlayer::play() {
  if (!_fileLoaded || _eventCount == 0) return;

  if (_state == PLAYER_PAUSED) {
    // Reprendre depuis la position de pause
    _playbackStartMs = millis() - _pausePositionMs;
  } else {
    // Demarrer du debut
    _currentEvent = 0;
    _playbackStartMs = millis();
  }
  _state = PLAYER_PLAYING;

  if (DEBUG) {
    Serial.println("DEBUG: MidiFilePlayer - PLAY");
  }
}

void MidiFilePlayer::pause() {
  if (_state == PLAYER_PLAYING) {
    _pausePositionMs = millis() - _playbackStartMs;
    _state = PLAYER_PAUSED;

    // Arreter toutes les notes en cours
    if (_instrument != nullptr) {
      _instrument->allSoundOff();
    }

    if (DEBUG) {
      Serial.print("DEBUG: MidiFilePlayer - PAUSE a ");
      Serial.print(_pausePositionMs);
      Serial.println("ms");
    }
  }
}

void MidiFilePlayer::stop() {
  if (_state != PLAYER_STOPPED) {
    _state = PLAYER_STOPPED;
    _currentEvent = 0;
    _pausePositionMs = 0;

    if (_instrument != nullptr) {
      _instrument->allSoundOff();
    }

    if (DEBUG) {
      Serial.println("DEBUG: MidiFilePlayer - STOP");
    }
  }
}

void MidiFilePlayer::update() {
  if (_state != PLAYER_PLAYING || _instrument == nullptr) return;

  uint32_t currentPositionMs = millis() - _playbackStartMs;

  // Traiter tous les evenements dont le temps est atteint
  while (_currentEvent < _eventCount) {
    MidiFileEvent& evt = _events[_currentEvent];

    if (evt.absoluteTimeMs > currentPositionMs) {
      break;  // Pas encore le moment
    }

    // Filtre canal (255 = tous)
    if (_channelFilter != 255 && evt.channel != _channelFilter) {
      _currentEvent++;
      continue;
    }

    // Dispatcher l'evenement
    uint8_t msgType = evt.type & 0xF0;
    switch (msgType) {
      case 0x90:  // Note On
        if (evt.data2 > 0) {
          _instrument->noteOn(evt.data1, evt.data2);
        } else {
          _instrument->noteOff(evt.data1);
        }
        break;

      case 0x80:  // Note Off
        _instrument->noteOff(evt.data1);
        break;

      case 0xB0:  // Control Change
        _instrument->handleControlChange(evt.data1, evt.data2);
        break;
    }

    _currentEvent++;
  }

  // Fin du fichier
  if (_currentEvent >= _eventCount) {
    stop();
  }
}

// --- Getters ---

PlayerState MidiFilePlayer::getState() const { return _state; }
uint16_t MidiFilePlayer::getEventCount() const { return _eventCount; }
uint32_t MidiFilePlayer::getDurationMs() const { return _durationMs; }
String MidiFilePlayer::getFileName() const { return _fileName; }
bool MidiFilePlayer::isFileLoaded() const { return _fileLoaded; }

MidiLoadError MidiFilePlayer::getLoadError() const { return _loadError; }

const char* MidiFilePlayer::getLoadErrorCode() const {
  switch (_loadError) {
    case MIDI_LOAD_OK:              return "ok";
    case MIDI_LOAD_ERR_OPEN:        return "open_failed";
    case MIDI_LOAD_ERR_HEADER:     return "invalid_header";
    case MIDI_LOAD_ERR_SMPTE:      return "smpte_unsupported";
    case MIDI_LOAD_ERR_DIVISION:   return "invalid_division";
    case MIDI_LOAD_ERR_FORMAT2:    return "format2_unsupported";
    case MIDI_LOAD_ERR_EVENT_LIMIT: return "event_limit_exceeded";
    case MIDI_LOAD_ERR_NO_EVENTS:  return "no_events";
  }
  return "unknown";
}

void MidiFilePlayer::setChannelFilter(uint8_t channel) { _channelFilter = channel; }
uint8_t MidiFilePlayer::getChannelFilter() const { return _channelFilter; }
uint16_t MidiFilePlayer::getActiveChannels() const { return _activeChannels; }

uint32_t MidiFilePlayer::getPositionMs() const {
  if (_state == PLAYER_PLAYING) {
    return millis() - _playbackStartMs;
  } else if (_state == PLAYER_PAUSED) {
    return _pausePositionMs;
  }
  return 0;
}

float MidiFilePlayer::getProgressPercent() const {
  if (_durationMs == 0) return 0.0;
  return (float)getPositionMs() / (float)_durationMs * 100.0;
}

// --- Parsing MIDI ---

bool MidiFilePlayer::parseFile(File& file) {
  // Lire header MThd
  uint16_t format, numTracks, division;
  if (!parseMThd(file, format, numTracks, division)) {
    return false;
  }

  if (DEBUG) {
    Serial.print("DEBUG: MIDI Format: ");
    Serial.print(format);
    Serial.print(", Pistes: ");
    Serial.print(numTracks);
    Serial.print(", Division: ");
    Serial.println(division);
  }

  // SMF Type 2 : chaque piste est une sequence temporelle INDEPENDANTE. Les
  // fusionner sur une seule timeline (comme Type 0/1) produirait un charabia ->
  // rejeter explicitement (audit : format 2 non gere).
  if (format == 2 && numTracks > 1) {
    _loadError = MIDI_LOAD_ERR_FORMAT2;
    if (DEBUG) {
      Serial.println("ERREUR: SMF Type 2 (sequences independantes) non supporte");
    }
    return false;
  }

  // Initialiser la carte de tempo GLOBALE : tous les changements de tempo de
  // toutes les pistes y sont accumules (en ticks absolus) avant conversion.
  _tempoMap.begin(division);

  // Passe 1 : lire chaque piste, evenements stockes en TICKS absolus.
  // On continue de parcourir les pistes meme apres avoir atteint la limite
  // d'evenements, afin de detecter la troncature avec precision (insertEvent
  // positionne _truncated) et de collecter TOUS les changements de tempo.
  for (uint16_t t = 0; t < numTracks; t++) {
    // Chercher le chunk MTrk
    uint8_t chunkId[4];
    if (file.read(chunkId, 4) != 4) break;

    if (chunkId[0] != 'M' || chunkId[1] != 'T' || chunkId[2] != 'r' || chunkId[3] != 'k') {
      // Chunk inconnu, lire la taille et sauter
      uint32_t chunkLen = readU32(file);
      file.seek(file.position() + chunkLen);
      t--;  // Reessayer pour trouver MTrk
      continue;
    }

    uint32_t trackLength = readU32(file);

    if (DEBUG) {
      Serial.print("DEBUG: Piste ");
      Serial.print(t);
      Serial.print(": ");
      Serial.print(trackLength);
      Serial.println(" octets");
    }

    if (!parseMTrk(file, trackLength)) {
      if (DEBUG) {
        Serial.print("ERREUR: Echec parsing piste ");
        Serial.println(t);
      }
    }
  }

  return true;
}

bool MidiFilePlayer::parseMThd(File& file, uint16_t& format, uint16_t& numTracks, uint16_t& division) {
  uint8_t header[4];
  if (file.read(header, 4) != 4) { _loadError = MIDI_LOAD_ERR_HEADER; return false; }
  if (header[0] != 'M' || header[1] != 'T' || header[2] != 'h' || header[3] != 'd') {
    _loadError = MIDI_LOAD_ERR_HEADER;
    return false;
  }

  uint32_t headerLen = readU32(file);
  if (headerLen < 6) { _loadError = MIDI_LOAD_ERR_HEADER; return false; }

  format = readU16(file);
  numTracks = readU16(file);
  division = readU16(file);

  // Sauter les octets supplementaires si headerLen > 6
  if (headerLen > 6) {
    file.seek(file.position() + (headerLen - 6));
  }

  // On ne supporte que les divisions en ticks/beat (bit 15 = 0)
  if (division & 0x8000) {
    _loadError = MIDI_LOAD_ERR_SMPTE;
    if (DEBUG) {
      Serial.println("ERREUR: SMPTE time division non supporte");
    }
    return false;
  }

  // Garde: une division nulle (ticks/beat = 0) provoquerait un divide-by-zero
  // dans la conversion tick->ms sur fichier corrompu/malforme -> rejeter.
  if (division == 0) {
    _loadError = MIDI_LOAD_ERR_DIVISION;
    if (DEBUG) {
      Serial.println("ERREUR: Division ticks/beat nulle (fichier MIDI invalide)");
    }
    return false;
  }

  return true;
}

bool MidiFilePlayer::parseMTrk(File& file, uint32_t trackLength) {
  uint32_t trackEnd = file.position() + trackLength;
  uint32_t currentTick = 0;
  uint8_t runningStatus = 0;

  // Passe 1 : on stocke les evenements en TICKS absolus (dans absoluteTimeMs).
  // Les changements de tempo sont pousses dans la carte GLOBALE _tempoMap et
  // seront appliques a toutes les pistes lors de la conversion (passe 2). On ne
  // borne PLUS la boucle par _eventCount : insertEvent gere le depassement et
  // signale la troncature, et on doit lire toute la piste de tempo (piste 0)
  // meme si les pistes de notes ont deja rempli le tableau.
  while (file.position() < trackEnd) {
    // Lire delta time (VLQ)
    uint32_t bytesRead = 0;
    uint32_t deltaTime = readVLQ(file, bytesRead);
    currentTick += deltaTime;

    // Lire le premier octet de l'evenement
    // Garde EOF : sur un fichier tronque (trackEnd > taille reelle), file.read()
    // retourne -1 sans faire avancer la position -> sortir pour eviter une boucle infinie.
    int statusRaw = file.read();
    if (statusRaw < 0) break;
    uint8_t statusByte = (uint8_t)statusRaw;

    // Running status : si le bit 7 n'est pas set, c'est un data byte
    if (statusByte < 0x80) {
      // Running status : le statusByte est en fait data1
      if (runningStatus == 0) {
        continue;  // Pas de running status, erreur
      }
      // Remettre le byte et utiliser le running status
      uint8_t data1 = statusByte;
      statusByte = runningStatus;

      uint8_t msgType = statusByte & 0xF0;
      uint8_t channel = statusByte & 0x0F;

      if (msgType == 0x80 || msgType == 0x90 || msgType == 0xB0) {
        uint8_t data2 = file.read();
        MidiFileEvent evt;
        evt.absoluteTimeMs = currentTick;  // tick (converti en ms en passe 2)
        evt.type = statusByte;
        evt.channel = channel;
        evt.data1 = data1;
        evt.data2 = data2;
        insertEvent(evt);
      } else if (msgType == 0xC0 || msgType == 0xD0) {
        // Program Change, Channel Pressure : 1 data byte (deja lu)
      }
      continue;
    }

    // Mettre a jour running status pour les channel messages
    if (statusByte >= 0x80 && statusByte < 0xF0) {
      runningStatus = statusByte;
    }

    uint8_t msgType = statusByte & 0xF0;
    uint8_t channel = statusByte & 0x0F;

    switch (statusByte) {
      case 0xFF: {
        // Meta event
        uint8_t metaType = file.read();
        uint32_t br = 0;
        uint32_t metaLen = readVLQ(file, br);

        if (metaType == 0x51 && metaLen == 3) {
          // Tempo change : 3 octets = us/quarter note. Enregistre dans la carte
          // globale au tick absolu courant (applique a TOUTES les pistes).
          uint32_t newTempo = 0;
          newTempo = (uint32_t)file.read() << 16;
          newTempo |= (uint32_t)file.read() << 8;
          newTempo |= (uint32_t)file.read();

          _tempoMap.addTempoChange(currentTick, newTempo);

          if (DEBUG) {
            float bpm = (newTempo > 0) ? (60000000.0 / newTempo) : 0.0;
            Serial.print("DEBUG: Tempo change: ");
            Serial.print(bpm, 1);
            Serial.print(" BPM a tick=");
            Serial.println(currentTick);
          }
        } else if (metaType == 0x2F) {
          // End of Track
          return true;
        } else {
          // Sauter les meta events inconnus
          file.seek(file.position() + metaLen);
        }
        break;
      }

      case 0xF0:
      case 0xF7: {
        // SysEx : lire longueur et sauter
        uint32_t br = 0;
        uint32_t sysexLen = readVLQ(file, br);
        file.seek(file.position() + sysexLen);
        break;
      }

      default: {
        if (msgType == 0x80 || msgType == 0x90 || msgType == 0xB0 ||
            msgType == 0xA0 || msgType == 0xE0) {
          // Messages a 2 data bytes
          uint8_t data1 = file.read();
          uint8_t data2 = file.read();

          // On ne garde que Note On/Off et CC
          if (msgType == 0x80 || msgType == 0x90 || msgType == 0xB0) {
            MidiFileEvent evt;
            evt.absoluteTimeMs = currentTick;  // tick (converti en ms en passe 2)
            evt.type = statusByte;
            evt.channel = channel;
            evt.data1 = data1;
            evt.data2 = data2;
            insertEvent(evt);
          }
        } else if (msgType == 0xC0 || msgType == 0xD0) {
          // Messages a 1 data byte
          file.read();  // Lire et ignorer
        }
        break;
      }
    }
  }

  // Si on n'a pas atteint la fin du track, seek a la fin
  if (file.position() < trackEnd) {
    file.seek(trackEnd);
  }

  return true;
}

uint32_t MidiFilePlayer::readVLQ(File& file, uint32_t& bytesRead) {
  uint32_t value = 0;
  bytesRead = 0;

  // Un VLQ MIDI valide ne depasse jamais 4 octets. La limite et le check EOF
  // evitent une boucle infinie sur fichier tronque/corrompu : a l'EOF,
  // file.read() retourne -1, casté en uint8_t = 0xFF (bit 0x80 toujours actif).
  do {
    int raw = file.read();
    if (raw < 0) break;  // EOF : ne pas boucler sur 0xFF
    uint8_t b = (uint8_t)raw;
    bytesRead++;
    value = (value << 7) | (b & 0x7F);
    if ((b & 0x80) == 0) break;
  } while (bytesRead < 4);

  return value;
}

uint16_t MidiFilePlayer::readU16(File& file) {
  uint8_t buf[2];
  file.read(buf, 2);
  return ((uint16_t)buf[0] << 8) | buf[1];
}

uint32_t MidiFilePlayer::readU32(File& file) {
  uint8_t buf[4];
  file.read(buf, 4);
  return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
         ((uint32_t)buf[2] << 8) | buf[3];
}

void MidiFilePlayer::insertEvent(const MidiFileEvent& evt) {
  if (_eventCount >= MIDI_FILE_MAX_EVENTS) {
    // Capacite atteinte : on ne peut pas tout stocker -> fichier trop dense.
    // On marque la troncature ; loadFile() refusera le fichier au lieu de n'en
    // jouer qu'une partie sans prevenir (audit P0).
    _truncated = true;
    return;
  }
  _events[_eventCount] = evt;
  _eventCount++;
}

void MidiFilePlayer::convertTicksToMs() {
  // Les evenements portent des TICKS absolus. On finalise la carte de tempo
  // globale, on trie par tick (ordre temporel reel, toutes pistes confondues),
  // puis on convertit chaque tick en ms. tickToMs() etant monotone croissant,
  // l'ordre par tick reste l'ordre par ms.
  _tempoMap.finalize();
  sortEvents();
  for (uint16_t i = 0; i < _eventCount; i++) {
    _events[i].absoluteTimeMs = _tempoMap.tickToMs(_events[i].absoluteTimeMs);
  }
}

void MidiFilePlayer::sortEvents() {
  // Simple insertion sort stable (suffisant pour <= MIDI_FILE_MAX_EVENTS).
  // Trie sur la valeur de absoluteTimeMs, qui contient le tick a ce stade.
  for (uint16_t i = 1; i < _eventCount; i++) {
    MidiFileEvent temp = _events[i];
    int32_t j = (int32_t)i - 1;
    while (j >= 0 && _events[j].absoluteTimeMs > temp.absoluteTimeMs) {
      _events[j + 1] = _events[j];
      j--;
    }
    _events[j + 1] = temp;
  }
}

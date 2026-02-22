#include "MidiFilePlayer.h"
#include "InstrumentManager.h"

MidiFilePlayer::MidiFilePlayer()
  : _instrument(nullptr), _state(PLAYER_STOPPED),
    _events(nullptr), _eventCount(0), _currentEvent(0),
    _durationMs(0), _playbackStartMs(0), _pausePositionMs(0),
    _fileLoaded(false), _channelFilter(255), _activeChannels(0) {
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

  File file = LittleFS.open(path, "r");
  if (!file) {
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

  if (success && _eventCount > 0) {
    // Trier les evenements par temps (necessaire pour multi-pistes)
    sortEvents();
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
  } else {
    if (DEBUG) {
      Serial.println("ERREUR: MidiFilePlayer - Echec parsing");
    }
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

  // Tempo par defaut : 120 BPM = 500000 us/beat
  uint32_t baseTempo = 500000;

  // Lire chaque piste
  for (uint16_t t = 0; t < numTracks && _eventCount < MIDI_FILE_MAX_EVENTS; t++) {
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

    if (!parseMTrk(file, trackLength, division, baseTempo)) {
      if (DEBUG) {
        Serial.print("ERREUR: Echec parsing piste ");
        Serial.println(t);
      }
    }
  }

  return _eventCount > 0;
}

bool MidiFilePlayer::parseMThd(File& file, uint16_t& format, uint16_t& numTracks, uint16_t& division) {
  uint8_t header[4];
  if (file.read(header, 4) != 4) return false;
  if (header[0] != 'M' || header[1] != 'T' || header[2] != 'h' || header[3] != 'd') return false;

  uint32_t headerLen = readU32(file);
  if (headerLen < 6) return false;

  format = readU16(file);
  numTracks = readU16(file);
  division = readU16(file);

  // Sauter les octets supplementaires si headerLen > 6
  if (headerLen > 6) {
    file.seek(file.position() + (headerLen - 6));
  }

  // On ne supporte que les divisions en ticks/beat (bit 15 = 0)
  if (division & 0x8000) {
    if (DEBUG) {
      Serial.println("ERREUR: SMPTE time division non supporte");
    }
    return false;
  }

  return true;
}

bool MidiFilePlayer::parseMTrk(File& file, uint32_t trackLength, uint16_t division, uint32_t baseTempo) {
  uint32_t trackEnd = file.position() + trackLength;
  uint32_t currentTick = 0;
  uint32_t currentTempo = baseTempo;
  uint8_t runningStatus = 0;

  // Pour la conversion temps : on accumule les ms au fur et a mesure des changements de tempo
  uint32_t lastTempoChangeTick = 0;
  uint32_t lastTempoChangeMs = 0;

  while (file.position() < trackEnd && _eventCount < MIDI_FILE_MAX_EVENTS) {
    // Lire delta time (VLQ)
    uint32_t bytesRead = 0;
    uint32_t deltaTime = readVLQ(file, bytesRead);
    currentTick += deltaTime;

    // Convertir le tick courant en ms
    uint32_t ticksSinceTempoChange = currentTick - lastTempoChangeTick;
    uint32_t currentTimeMs = lastTempoChangeMs + ticksToMs(ticksSinceTempoChange, currentTempo, division);

    // Lire le premier octet de l'evenement
    uint8_t statusByte = file.read();

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
        evt.absoluteTimeMs = currentTimeMs;
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
          // Tempo change : 3 octets = us/quarter note
          uint32_t newTempo = 0;
          newTempo = (uint32_t)file.read() << 16;
          newTempo |= (uint32_t)file.read() << 8;
          newTempo |= (uint32_t)file.read();

          // Mettre a jour le point de reference tempo
          lastTempoChangeMs = currentTimeMs;
          lastTempoChangeTick = currentTick;
          currentTempo = newTempo;

          if (DEBUG) {
            float bpm = 60000000.0 / newTempo;
            Serial.print("DEBUG: Tempo change: ");
            Serial.print(bpm, 1);
            Serial.print(" BPM a t=");
            Serial.print(currentTimeMs);
            Serial.println("ms");
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
            evt.absoluteTimeMs = currentTimeMs;
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
  uint8_t b;

  do {
    b = file.read();
    bytesRead++;
    value = (value << 7) | (b & 0x7F);
  } while (b & 0x80);

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
  if (_eventCount >= MIDI_FILE_MAX_EVENTS) return;
  _events[_eventCount] = evt;
  _eventCount++;
}

uint32_t MidiFilePlayer::ticksToMs(uint32_t ticks, uint32_t tempo, uint16_t division) {
  // tempo = microsecondes par quarter note
  // division = ticks par quarter note
  // ms = ticks * (tempo / division) / 1000
  return (uint32_t)((uint64_t)ticks * tempo / division / 1000);
}

void MidiFilePlayer::sortEvents() {
  // Simple insertion sort (suffisant pour < 2000 elements)
  for (uint16_t i = 1; i < _eventCount; i++) {
    MidiFileEvent temp = _events[i];
    int16_t j = i - 1;
    while (j >= 0 && _events[j].absoluteTimeMs > temp.absoluteTimeMs) {
      _events[j + 1] = _events[j];
      j--;
    }
    _events[j + 1] = temp;
  }
}

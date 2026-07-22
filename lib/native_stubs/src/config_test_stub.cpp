#include "ConfigStorage.h"
RuntimeConfig cfg{};

// Native test stub: the real ConfigStorage.cpp (LittleFS/ArduinoJson) is excluded
// from the host build, but AutoCalibrator::applyResults() persists via save().
bool ConfigStorage::save() { return true; }

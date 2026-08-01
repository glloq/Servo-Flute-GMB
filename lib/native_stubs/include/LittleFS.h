// Minimal in-memory LittleFS/File stub for host unit tests.
//
// The real ESP32 LittleFS is unavailable on the host. This stub reproduces just
// the File API MidiFilePlayer uses (read, seek, position, size, close) backed by
// an in-memory byte buffer. Tests register file contents with
// LittleFS.__put(path, bytes) and the parser reads them back through the same
// calls it makes on-device.
#ifndef LITTLEFS_NATIVE_STUB_H
#define LITTLEFS_NATIVE_STUB_H

#include <Arduino.h>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

class __FsFile {
public:
  __FsFile() : _data(nullptr), _pos(0), _ok(false) {}
  explicit __FsFile(const std::vector<uint8_t>* d)
    : _data(d), _pos(0), _ok(d != nullptr) {}

  explicit operator bool() const { return _ok; }

  // Single byte, or -1 at EOF (matches Arduino Stream::read()).
  int read() {
    if (!_data || _pos >= _data->size()) return -1;
    return (int)(*_data)[_pos++];
  }

  // Block read: returns number of bytes actually copied.
  int read(uint8_t* buf, size_t len) {
    if (!_data) return 0;
    size_t n = 0;
    while (n < len && _pos < _data->size()) buf[n++] = (*_data)[_pos++];
    return (int)n;
  }

  uint32_t position() const { return (uint32_t)_pos; }
  bool seek(uint32_t p) { _pos = (size_t)p; return true; }
  uint32_t size() const { return _data ? (uint32_t)_data->size() : 0; }
  void close() {}
  bool isDirectory() const { return false; }

private:
  const std::vector<uint8_t>* _data;
  size_t _pos;
  bool _ok;
};

typedef __FsFile File;

class __LittleFS {
public:
  File open(const char* path, const char* mode = "r") {
    (void)mode;
    auto it = _files.find(std::string(path));
    if (it == _files.end()) return File();
    return File(&it->second);
  }
  bool exists(const char* path) { return _files.count(std::string(path)) > 0; }
  bool remove(const char* path) { return _files.erase(std::string(path)) > 0; }

  // Test helper: register/replace a file's bytes.
  void __put(const std::string& path, const std::vector<uint8_t>& data) {
    _files[path] = data;
  }
  void __clear() { _files.clear(); }

private:
  std::map<std::string, std::vector<uint8_t>> _files;
};

extern __LittleFS LittleFS;

#endif

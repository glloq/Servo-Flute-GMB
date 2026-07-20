#pragma once
#include <cstdint>
#include <map>
class WireClass {
public:
  void beginTransmission(uint8_t addr){ _addr=addr; }
  void write(uint8_t v){ _lastWrite=v; }
  uint8_t endTransmission(bool stop=true);
  uint8_t requestFrom(uint8_t addr,uint8_t count){ _addr=addr; _requestCount=count; return available(); }
  int available();
  uint8_t read();
  void setPresent(uint8_t addr, bool present){ presentMap[addr]=present; }
  void clear(){ presentMap.clear(); readMap.clear(); }
  std::map<uint8_t,bool> presentMap;
  std::map<uint8_t,uint8_t> readMap;
private:
  uint8_t _addr=0, _lastWrite=0, _requestCount=0;
};
extern WireClass Wire;

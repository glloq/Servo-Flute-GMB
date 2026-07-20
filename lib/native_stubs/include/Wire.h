#pragma once
#include <cstdint>
class WireClass { public: void beginTransmission(uint8_t){} void write(uint8_t){} uint8_t endTransmission(bool=true){return 0;} uint8_t requestFrom(uint8_t,uint8_t){return 0;} int available(){return 0;} uint8_t read(){return 0;} };
extern WireClass Wire;

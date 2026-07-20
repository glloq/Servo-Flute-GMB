#pragma once
#include <cstdint>
class Adafruit_PWMServoDriver {
public:
  explicit Adafruit_PWMServoDriver(uint8_t addr = 0x40) : _addr(addr), begun(false) {}
  bool begin() { begun = true; return true; }
  void setPWMFreq(float) {}
  void setPWM(uint8_t channel, uint16_t on, uint16_t off);
  uint8_t address() const { return _addr; }
  bool begun;
private:
  uint8_t _addr;
};
extern int __pwm_write_count;

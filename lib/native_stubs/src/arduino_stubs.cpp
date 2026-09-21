#include "Arduino.h"
#include "Wire.h"
#include "Adafruit_PWMServoDriver.h"
#include "LittleFS.h"
#include <map>
unsigned long __test_millis = 0;
__LittleFS LittleFS;
SerialClass Serial;
WireClass Wire;
std::map<uint8_t,int> __analog_writes, __digital_writes, __analog_reads, __digital_reads;
long map(long x,long in_min,long in_max,long out_min,long out_max){ return (x-in_min)*(out_max-out_min)/(in_max-in_min)+out_min; }
void pinMode(uint8_t,uint8_t){}
void digitalWrite(uint8_t pin,uint8_t value){ __digital_writes[pin]=value; }
int digitalRead(uint8_t pin){ return __digital_reads[pin]; }
void analogWrite(uint8_t pin,int value){ __analog_writes[pin]=value; }
int analogRead(uint8_t pin){ return __analog_reads[pin]; }

uint8_t WireClass::endTransmission(bool){
  // Memorise le registre adresse par cette transmission, pour que la lecture qui
  // suit rende la valeur de CE registre (et journalise les ecritures).
  if (_tx.size() >= 1) { _lastReg8 = _tx[0]; _reg8Valid = true; }
  if (_tx.size() >= 2) { _lastReg16 = (uint16_t)((_tx[0] << 8) | _tx[1]); }
  if (_tx.size() == 2) writeLog.push_back({{_addr, _tx[0]}, _tx[1]});
  auto it=presentMap.find(_addr);
  return (it!=presentMap.end() && it->second) ? 0 : 4;
}
int WireClass::available(){
  auto it=presentMap.find(_addr);
  if (it==presentMap.end() || !it->second) return 0;
  return (_readOffset < _requestCount) ? (_requestCount - _readOffset) : 0;
}
uint8_t WireClass::read(){
  uint8_t value = 0;
  auto def = readMap.find(_addr);
  if (def != readMap.end()) value = def->second;
  // Registre 16 bits (VL6180X) puis registre 8 bits (VL53L0X / PCA9685).
  auto r16 = reg16Map.find({_addr, (uint16_t)(_lastReg16 + _readOffset)});
  if (r16 != reg16Map.end()) value = r16->second;
  else if (_reg8Valid) {
    auto r8 = reg8Map.find({_addr, (uint8_t)(_lastReg8 + _readOffset)});
    if (r8 != reg8Map.end()) value = r8->second;
  }
  _readOffset++;
  return value;
}
int __pwm_write_count = 0;
void Adafruit_PWMServoDriver::setPWM(uint8_t, uint16_t, uint16_t){ __pwm_write_count++; }

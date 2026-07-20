#include "Arduino.h"
#include "Wire.h"
#include <map>
unsigned long __test_millis = 0;
SerialClass Serial;
WireClass Wire;
std::map<uint8_t,int> __analog_writes, __digital_writes, __analog_reads, __digital_reads;
long map(long x,long in_min,long in_max,long out_min,long out_max){ return (x-in_min)*(out_max-out_min)/(in_max-in_min)+out_min; }
void pinMode(uint8_t,uint8_t){}
void digitalWrite(uint8_t pin,uint8_t value){ __digital_writes[pin]=value; }
int digitalRead(uint8_t pin){ return __digital_reads[pin]; }
void analogWrite(uint8_t pin,int value){ __analog_writes[pin]=value; }
int analogRead(uint8_t pin){ return __analog_reads[pin]; }

#pragma once
#include <cstdint>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <string>
using byte = uint8_t;
#define HIGH 1
#define LOW 0
#define OUTPUT 1
#define INPUT 0
#define INPUT_PULLUP 2
#define HEX 16
#define PROGMEM
#define pgm_read_byte(addr) (*(const int8_t*)(addr))
extern unsigned long __test_millis;
inline unsigned long millis(){ return __test_millis; }
inline void delay(unsigned long ms){ __test_millis += ms; }
long map(long x,long in_min,long in_max,long out_min,long out_max);
template<typename T, typename A, typename B> T constrain(T x,A a,B b){ T aa=(T)a, bb=(T)b; return x<aa?aa:(x>bb?bb:x); }
void pinMode(uint8_t pin,uint8_t mode);
void digitalWrite(uint8_t pin,uint8_t value);
int digitalRead(uint8_t pin);
void analogWrite(uint8_t pin,int value);
int analogRead(uint8_t pin);
class String: public std::string { public: using std::string::string; String():std::string(){} String(const char*s):std::string(s?s:""){} String(const std::string& s):std::string(s){} String(int v):std::string(std::to_string(v)){} bool isEmpty() const { return empty(); }};
struct SerialClass { template<class T> void print(const T&){} template<class T, class U> void print(const T&, const U&){} template<class T> void println(const T&){} template<class T, class U> void println(const T&, const U&){} void println(){} };
extern SerialClass Serial;

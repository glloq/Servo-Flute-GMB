#pragma once
#include <cstdint>
#include <cstddef>
#include <map>
#include <utility>
#include <vector>

// Stub I2C hote.
//
// Il reproduit assez du bus pour tester un vrai pilote a registres (VL53L0X /
// VL6180X) : les octets ecrits dans une transmission sont conserves, donc la
// lecture qui suit sait QUEL registre a ete adresse et peut rendre une valeur
// par registre. Sans cela, toute lecture rendait le meme octet et un pilote
// n'etait pas distinguable d'un capteur muet.
class WireClass {
public:
  void beginTransmission(uint8_t addr) { _addr = addr; _tx.clear(); }
  void write(uint8_t v) { _lastWrite = v; _tx.push_back(v); }
  uint8_t endTransmission(bool stop = true);
  uint8_t requestFrom(uint8_t addr, uint8_t count) {
    _addr = addr; _requestCount = count; _readOffset = 0; return available();
  }
  int available();
  uint8_t read();

  void setPresent(uint8_t addr, bool present) { presentMap[addr] = present; }
  void clear() {
    presentMap.clear(); readMap.clear(); reg8Map.clear(); reg16Map.clear();
    writeLog.clear(); _tx.clear(); _lastReg8 = 0; _lastReg16 = 0; _reg8Valid = false;
  }

  // Valeur rendue pour un registre 8 bits (VL53L0X, PCA9685).
  void setReg8(uint8_t addr, uint8_t reg, uint8_t value) { reg8Map[{addr, reg}] = value; }
  // Valeur rendue pour un registre 16 bits (VL6180X).
  void setReg16(uint8_t addr, uint16_t reg, uint8_t value) { reg16Map[{addr, reg}] = value; }

  std::map<uint8_t, bool> presentMap;      // adresse -> repond a un ACK
  std::map<uint8_t, uint8_t> readMap;      // valeur par defaut par adresse
  std::map<std::pair<uint8_t, uint8_t>, uint8_t> reg8Map;
  std::map<std::pair<uint8_t, uint16_t>, uint8_t> reg16Map;
  // Journal des ecritures de registres 8 bits (adresse, registre, valeur).
  std::vector<std::pair<std::pair<uint8_t, uint8_t>, uint8_t>> writeLog;

private:
  uint8_t _addr = 0, _lastWrite = 0, _requestCount = 0;
  std::vector<uint8_t> _tx;
  uint8_t _lastReg8 = 0;
  uint16_t _lastReg16 = 0;
  bool _reg8Valid = false;
  uint8_t _readOffset = 0;
};
extern WireClass Wire;

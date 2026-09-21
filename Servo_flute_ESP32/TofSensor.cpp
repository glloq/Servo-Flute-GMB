#include "TofSensor.h"
#include "settings.h"
#include <Wire.h>

// --- Registres VL53L0X (jeu 8 bits) ---
#define VL_SYSRANGE_START                              0x00
#define VL_SYSTEM_SEQUENCE_CONFIG                      0x01
#define VL_SYSTEM_INTERRUPT_CONFIG_GPIO                0x0A
#define VL_SYSTEM_INTERRUPT_CLEAR                      0x0B
#define VL_RESULT_INTERRUPT_STATUS                     0x13
#define VL_RESULT_RANGE_STATUS                         0x14
#define VL_MSRC_CONFIG_CONTROL                         0x60
#define VL_FINAL_RANGE_CONFIG_MIN_COUNT_RATE_RTN_LIMIT 0x44
#define VL_DYNAMIC_SPAD_NUM_REQUESTED_REF_SPAD         0x4E
#define VL_DYNAMIC_SPAD_REF_EN_START_OFFSET            0x4F
#define VL_GLOBAL_CONFIG_SPAD_ENABLES_REF_0            0xB0
#define VL_GLOBAL_CONFIG_REF_EN_START_SELECT           0xB6
#define VL_IDENTIFICATION_MODEL_ID                     0xC0
#define VL_GPIO_HV_MUX_ACTIVE_HIGH                     0x84
#define VL53L0X_MODEL_ID                               0xEE

// --- Registres VL6180X (jeu 16 bits) ---
#define VL6180X_IDENTIFICATION_MODEL_ID          0x0000
#define VL6180X_MODEL_ID                         0xB4
#define VL6180X_SYSTEM_FRESH_OUT_OF_RESET        0x0016
#define VL6180X_SYSRANGE_START                   0x0018
#define VL6180X_RESULT_RANGE_STATUS              0x004D
#define VL6180X_RESULT_RANGE_VAL                 0x0062
#define VL6180X_SYSTEM_INTERRUPT_CLEAR           0x0015
#define VL6180X_READOUT_AVERAGING_PERIOD         0x010A

// Bornes d'attente pendant l'initialisation UNIQUEMENT (au boot). Aucune de ces
// attentes n'a lieu en fonctionnement : la lecture de distance est non bloquante.
#define TOF_INIT_POLL_TIMEOUT_MS 250

namespace {

void writeReg8(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(TOF_I2C_ADDRESS);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission();
}

uint8_t readReg8(uint8_t reg) {
  Wire.beginTransmission(TOF_I2C_ADDRESS);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)TOF_I2C_ADDRESS, (uint8_t)1);
  return Wire.available() ? Wire.read() : 0;
}

uint16_t readReg16From8(uint8_t reg) {
  Wire.beginTransmission(TOF_I2C_ADDRESS);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)TOF_I2C_ADDRESS, (uint8_t)2);
  uint16_t value = 0;
  if (Wire.available() >= 2) {
    value = (uint16_t)(Wire.read() << 8);
    value |= Wire.read();
  }
  return value;
}

void writeReg16From8(uint8_t reg, uint16_t value) {
  Wire.beginTransmission(TOF_I2C_ADDRESS);
  Wire.write(reg);
  Wire.write((uint8_t)((value >> 8) & 0xFF));
  Wire.write((uint8_t)(value & 0xFF));
  Wire.endTransmission();
}

void writeMulti(uint8_t reg, const uint8_t* data, uint8_t len) {
  Wire.beginTransmission(TOF_I2C_ADDRESS);
  Wire.write(reg);
  for (uint8_t i = 0; i < len; i++) Wire.write(data[i]);
  Wire.endTransmission();
}

void readMulti(uint8_t reg, uint8_t* data, uint8_t len) {
  Wire.beginTransmission(TOF_I2C_ADDRESS);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)TOF_I2C_ADDRESS, len);
  for (uint8_t i = 0; i < len; i++) data[i] = Wire.available() ? Wire.read() : 0;
}

void writeReg16Addr(uint16_t reg, uint8_t value) {
  Wire.beginTransmission(TOF_I2C_ADDRESS);
  Wire.write((uint8_t)((reg >> 8) & 0xFF));
  Wire.write((uint8_t)(reg & 0xFF));
  Wire.write(value);
  Wire.endTransmission();
}

uint8_t readReg16Addr(uint16_t reg) {
  Wire.beginTransmission(TOF_I2C_ADDRESS);
  Wire.write((uint8_t)((reg >> 8) & 0xFF));
  Wire.write((uint8_t)(reg & 0xFF));
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)TOF_I2C_ADDRESS, (uint8_t)1);
  return Wire.available() ? Wire.read() : 0;
}

// Reglages de tuning par defaut de l'API ST. Sans eux le capteur mesure, mais
// avec des parametres de VCSEL / fenetres temporelles non calibres : les
// distances rendues ne sont pas exploitables.
const uint8_t kVl53l0xTuning[][2] = {
  {0xFF, 0x01}, {0x00, 0x00}, {0xFF, 0x00}, {0x09, 0x00}, {0x10, 0x00}, {0x11, 0x00},
  {0x24, 0x01}, {0x25, 0xFF}, {0x75, 0x00}, {0xFF, 0x01}, {0x4E, 0x2C}, {0x48, 0x00},
  {0x30, 0x20}, {0xFF, 0x00}, {0x30, 0x09}, {0x54, 0x00}, {0x31, 0x04}, {0x32, 0x03},
  {0x40, 0x83}, {0x46, 0x25}, {0x60, 0x00}, {0x27, 0x00}, {0x50, 0x06}, {0x51, 0x00},
  {0x52, 0x96}, {0x56, 0x08}, {0x57, 0x30}, {0x61, 0x00}, {0x62, 0x00}, {0x64, 0x00},
  {0x65, 0x00}, {0x66, 0xA0}, {0xFF, 0x01}, {0x22, 0x32}, {0x47, 0x14}, {0x49, 0xFF},
  {0x4A, 0x00}, {0xFF, 0x00}, {0x7A, 0x0A}, {0x7B, 0x00}, {0x78, 0x21}, {0xFF, 0x01},
  {0x23, 0x34}, {0x42, 0x00}, {0x44, 0xFF}, {0x45, 0x26}, {0x46, 0x05}, {0x40, 0x40},
  {0x0E, 0x06}, {0x20, 0x1A}, {0x43, 0x40}, {0xFF, 0x00}, {0x34, 0x03}, {0x35, 0x44},
  {0xFF, 0x01}, {0x31, 0x04}, {0x4B, 0x09}, {0x4C, 0x05}, {0x4D, 0x04}, {0xFF, 0x00},
  {0x44, 0x00}, {0x45, 0x20}, {0x47, 0x08}, {0x48, 0x28}, {0x67, 0x00}, {0x70, 0x04},
  {0x71, 0x01}, {0x72, 0xFE}, {0x76, 0x00}, {0x77, 0x00}, {0xFF, 0x01}, {0x0D, 0x01},
  {0xFF, 0x00}, {0x80, 0x01}, {0x01, 0xF8}, {0xFF, 0x01}, {0x8E, 0x01}, {0x00, 0x01},
  {0xFF, 0x00}, {0x80, 0x00}
};

bool deviceAcknowledges() {
  Wire.beginTransmission(TOF_I2C_ADDRESS);
  return Wire.endTransmission() == 0;
}

}  // namespace

TofSensor::TofSensor()
  : _model(TOF_MODEL_VL53L0X), _state(TOF_ABSENT), _presentOnBus(false),
    _ranging(false), _distanceMm(0), _lastRangeStatus(0xFF),
    _stopVariable(0), _timeoutCount(0) {}

const char* TofSensor::stateName(TofSensorState state) {
  switch (state) {
    case TOF_ABSENT:      return "absent";
    case TOF_UNSUPPORTED: return "unsupported_device";
    case TOF_INIT_FAILED: return "init_failed";
    case TOF_READY:       return "ready";
    case TOF_FAULT:       return "fault";
  }
  return "unknown";
}

const char* TofSensor::stateName() const { return stateName(_state); }

bool TofSensor::begin(TofSensorModel model) {
  _model = model;
  _ranging = false;
  _distanceMm = 0;
  _lastRangeStatus = 0xFF;
  _timeoutCount = 0;

  _presentOnBus = deviceAcknowledges();
  if (!_presentOnBus) {
    _state = TOF_ABSENT;
    if (DEBUG) Serial.println("DEBUG: TofSensor - aucun peripherique a 0x29");
    return false;
  }

  bool ok = (model == TOF_MODEL_VL6180X) ? initVl6180x() : initVl53l0x();
  if (DEBUG) {
    Serial.print("DEBUG: TofSensor - ");
    Serial.print(model == TOF_MODEL_VL6180X ? "VL6180X" : "VL53L0X");
    Serial.print(" : ");
    Serial.println(stateName());
  }
  return ok;
}

bool TofSensor::getSpadInfo(uint8_t& count, bool& isAperture) {
  writeReg8(0x80, 0x01);
  writeReg8(0xFF, 0x01);
  writeReg8(0x00, 0x00);
  writeReg8(0xFF, 0x06);
  writeReg8(0x83, (uint8_t)(readReg8(0x83) | 0x04));
  writeReg8(0xFF, 0x07);
  writeReg8(0x81, 0x01);
  writeReg8(0x80, 0x01);
  writeReg8(0x94, 0x6B);
  writeReg8(0x83, 0x00);

  unsigned long start = millis();
  while (readReg8(0x83) == 0x00) {
    if ((millis() - start) >= TOF_INIT_POLL_TIMEOUT_MS) return false;
  }
  writeReg8(0x83, 0x01);

  uint8_t tmp = readReg8(0x92);
  count = (uint8_t)(tmp & 0x7F);
  isAperture = ((tmp >> 7) & 0x01) != 0;

  writeReg8(0x81, 0x00);
  writeReg8(0xFF, 0x06);
  writeReg8(0x83, (uint8_t)(readReg8(0x83) & ~0x04));
  writeReg8(0xFF, 0x01);
  writeReg8(0x00, 0x01);
  writeReg8(0xFF, 0x00);
  writeReg8(0x80, 0x00);
  return true;
}

bool TofSensor::performSingleRefCalibration(uint8_t vhvInitByte) {
  writeReg8(VL_SYSRANGE_START, (uint8_t)(0x01 | vhvInitByte));
  unsigned long start = millis();
  while ((readReg8(VL_RESULT_INTERRUPT_STATUS) & 0x07) == 0) {
    if ((millis() - start) >= TOF_INIT_POLL_TIMEOUT_MS) return false;
  }
  writeReg8(VL_SYSTEM_INTERRUPT_CLEAR, 0x01);
  writeReg8(VL_SYSRANGE_START, 0x00);
  return true;
}

bool TofSensor::initVl53l0x() {
  // 1. Le composant doit reellement etre un VL53L0X. Un peripherique quelconque
  //    qui acquitte a 0x29 ne suffit pas a annoncer le support.
  if (readReg8(VL_IDENTIFICATION_MODEL_ID) != VL53L0X_MODEL_ID) {
    _state = TOF_UNSUPPORTED;
    return false;
  }

  // 2. Data init : mode 2V8, I2C standard, lecture de la "stop variable" qui doit
  //    etre reinjectee avant chaque mesure single-shot.
  writeReg8(0x89, (uint8_t)(readReg8(0x89) | 0x01));   // VHV_CONFIG ... EXTSUP_HV
  writeReg8(0x88, 0x00);
  writeReg8(0x80, 0x01);
  writeReg8(0xFF, 0x01);
  writeReg8(0x00, 0x00);
  _stopVariable = readReg8(0x91);
  writeReg8(0x00, 0x01);
  writeReg8(0xFF, 0x00);
  writeReg8(0x80, 0x00);

  // 3. Desactiver les controles de limite SIGNAL_RATE_MSRC / PRE_RANGE et fixer
  //    la limite de taux de signal du final range a 0.25 MCPS (valeur ST).
  writeReg8(VL_MSRC_CONFIG_CONTROL, (uint8_t)(readReg8(VL_MSRC_CONFIG_CONTROL) | 0x12));
  writeReg16From8(VL_FINAL_RANGE_CONFIG_MIN_COUNT_RATE_RTN_LIMIT, (uint16_t)(0.25f * (1 << 7)));
  writeReg8(VL_SYSTEM_SEQUENCE_CONFIG, 0xFF);

  // 4. SPAD de reference : sans cette etape le capteur utilise un masque de SPAD
  //    incoherent et les mesures n'ont pas de sens metrique.
  uint8_t spadCount = 0;
  bool spadIsAperture = false;
  if (!getSpadInfo(spadCount, spadIsAperture)) {
    _state = TOF_INIT_FAILED;
    return false;
  }

  uint8_t refSpadMap[6];
  readMulti(VL_GLOBAL_CONFIG_SPAD_ENABLES_REF_0, refSpadMap, 6);

  writeReg8(0xFF, 0x01);
  writeReg8(VL_DYNAMIC_SPAD_REF_EN_START_OFFSET, 0x00);
  writeReg8(VL_DYNAMIC_SPAD_NUM_REQUESTED_REF_SPAD, 0x2C);
  writeReg8(0xFF, 0x00);
  writeReg8(VL_GLOBAL_CONFIG_REF_EN_START_SELECT, 0xB4);

  uint8_t firstSpad = spadIsAperture ? 12 : 0;
  uint8_t enabled = 0;
  for (uint8_t i = 0; i < 48; i++) {
    uint8_t byteIndex = (uint8_t)(i / 8);
    uint8_t bitMask = (uint8_t)(1 << (i % 8));
    if (i < firstSpad || enabled == spadCount) {
      refSpadMap[byteIndex] = (uint8_t)(refSpadMap[byteIndex] & ~bitMask);
    } else if (refSpadMap[byteIndex] & bitMask) {
      enabled++;
    }
  }
  writeMulti(VL_GLOBAL_CONFIG_SPAD_ENABLES_REF_0, refSpadMap, 6);

  // 5. Tuning par defaut de l'API ST.
  for (size_t i = 0; i < sizeof(kVl53l0xTuning) / sizeof(kVl53l0xTuning[0]); i++) {
    writeReg8(kVl53l0xTuning[i][0], kVl53l0xTuning[i][1]);
  }

  // 6. Interruption "nouvelle mesure prete" : c'est elle que pollMeasurement()
  //    interroge, ce qui permet de rester non bloquant.
  writeReg8(VL_SYSTEM_INTERRUPT_CONFIG_GPIO, 0x04);
  writeReg8(VL_GPIO_HV_MUX_ACTIVE_HIGH, (uint8_t)(readReg8(VL_GPIO_HV_MUX_ACTIVE_HIGH) & ~0x10));
  writeReg8(VL_SYSTEM_INTERRUPT_CLEAR, 0x01);

  // 7. Calibrations de reference VHV puis phase. Sans elles, les premieres
  //    mesures derivent fortement avec la temperature.
  writeReg8(VL_SYSTEM_SEQUENCE_CONFIG, 0x01);
  if (!performSingleRefCalibration(0x40)) { _state = TOF_INIT_FAILED; return false; }
  writeReg8(VL_SYSTEM_SEQUENCE_CONFIG, 0x02);
  if (!performSingleRefCalibration(0x00)) { _state = TOF_INIT_FAILED; return false; }
  writeReg8(VL_SYSTEM_SEQUENCE_CONFIG, 0xE8);

  _state = TOF_READY;
  return true;
}

bool TofSensor::initVl6180x() {
  if (readReg16Addr(VL6180X_IDENTIFICATION_MODEL_ID) != VL6180X_MODEL_ID) {
    _state = TOF_UNSUPPORTED;
    return false;
  }

  // Sequence SR03 "mandatory private registers" de ST, appliquee une seule fois
  // apres un reset (bit FRESH_OUT_OF_RESET).
  if (readReg16Addr(VL6180X_SYSTEM_FRESH_OUT_OF_RESET) == 1) {
    static const uint16_t regs[] = {
      0x0207, 0x0208, 0x0096, 0x0097, 0x00E3, 0x00E4, 0x00E5, 0x00E6, 0x00E7,
      0x00F5, 0x00D9, 0x00DB, 0x00DC, 0x00DD, 0x009F, 0x00A3, 0x00B7, 0x00BB,
      0x00B2, 0x00CA, 0x0198, 0x01B0, 0x01AD, 0x00FF, 0x0100, 0x0199, 0x01A6,
      0x01AC, 0x01A7, 0x0030
    };
    static const uint8_t vals[] = {
      0x01, 0x01, 0x00, 0xFD, 0x00, 0x04, 0x02, 0x01, 0x03,
      0x02, 0x05, 0xCE, 0x03, 0xF8, 0x00, 0x3C, 0x00, 0x3C,
      0x09, 0x09, 0x01, 0x17, 0x00, 0x05, 0x05, 0x05, 0x1B,
      0x3E, 0x1F, 0x00
    };
    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
      writeReg16Addr(regs[i], vals[i]);
    }
    writeReg16Addr(VL6180X_READOUT_AVERAGING_PERIOD, 0x30);
    writeReg16Addr(VL6180X_SYSTEM_FRESH_OUT_OF_RESET, 0x00);
  }

  _state = TOF_READY;
  return true;
}

bool TofSensor::startMeasurement() {
  if (_state != TOF_READY) return false;
  if (_model == TOF_MODEL_VL6180X) {
    writeReg16Addr(VL6180X_SYSTEM_INTERRUPT_CLEAR, 0x07);
    writeReg16Addr(VL6180X_SYSRANGE_START, 0x01);
  } else {
    // Reinjecter la stop variable avant chaque single-shot (sequence ST).
    writeReg8(0x80, 0x01);
    writeReg8(0xFF, 0x01);
    writeReg8(0x00, 0x00);
    writeReg8(0x91, _stopVariable);
    writeReg8(0x00, 0x01);
    writeReg8(0xFF, 0x00);
    writeReg8(0x80, 0x00);
    writeReg8(VL_SYSRANGE_START, 0x01);
  }
  _ranging = true;
  return true;
}

bool TofSensor::pollMeasurement() {
  if (!_ranging || _state != TOF_READY) return false;

  if (_model == TOF_MODEL_VL6180X) {
    if ((readReg16Addr(VL6180X_RESULT_RANGE_STATUS) & 0x04) == 0) return false;
    _distanceMm = (uint16_t)readReg16Addr(VL6180X_RESULT_RANGE_VAL);
    writeReg16Addr(VL6180X_SYSTEM_INTERRUPT_CLEAR, 0x07);
    _lastRangeStatus = 11;   // le VL6180X n'expose pas le meme code; 11 = valide
    _ranging = false;
    _timeoutCount = 0;
    return true;
  }

  if ((readReg8(VL_RESULT_INTERRUPT_STATUS) & 0x07) == 0) return false;

  // RESULT_RANGE_STATUS porte le code de validite dans les bits 3-6 ; la distance
  // en millimetres est a l'offset +10. Un code != 11 signale une mesure rejetee
  // par le capteur (signal trop faible, hors plage, phase incoherente).
  uint8_t status = readReg8(VL_RESULT_RANGE_STATUS);
  _lastRangeStatus = (uint8_t)((status >> 3) & 0x0F);
  _distanceMm = readReg16From8((uint8_t)(VL_RESULT_RANGE_STATUS + 10));
  writeReg8(VL_SYSTEM_INTERRUPT_CLEAR, 0x01);
  _ranging = false;

  if (_lastRangeStatus != 11) {
    // Mesure rendue mais invalide : ce n'est pas une panne du capteur, la valeur
    // est simplement inexploitable. L'appelant la traite comme "pas de mesure".
    return false;
  }
  _timeoutCount = 0;
  return true;
}

void TofSensor::abortMeasurement() {
  _ranging = false;
}

void TofSensor::reportTimeout(uint16_t maxConsecutive) {
  _ranging = false;
  if (_timeoutCount < 0xFFFF) _timeoutCount++;
  if (_state == TOF_READY && _timeoutCount >= maxConsecutive) {
    _state = TOF_FAULT;
    if (DEBUG) Serial.println("ERREUR: TofSensor - capteur perdu (timeouts repetes) -> pompe coupee");
  }
}

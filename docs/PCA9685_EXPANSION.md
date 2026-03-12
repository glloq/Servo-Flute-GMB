# Extension Multi-PCA9685 : Etude technique

> **Statut : IMPLEMENTE** — Le support multi-PCA9685 est fonctionnel dans le firmware.
> La seconde carte (adresse 0x41) est detectee et initialisee automatiquement si un canal >= 16
> est present dans la configuration (doigts, airflow, valve ou angle servo).
> L'interface web supporte les canaux 0-31 pour tous les selecteurs PCA.

## Contexte

Le systeme utilise actuellement un seul PCA9685 (16 canaux PWM via I2C, adresse 0x40).
Avec 1 canal reserve pour l'airflow, cela limite a **15 servos doigts maximum**.

Pour supporter des instruments avec plus de 15 trous (clarinette 17 cles, saxophone 23 cles),
cette etude prevoit l'ajout d'une **seconde carte PCA9685** (adresse 0x41).

---

## 1. Principe : canal virtuel 0-31

Le numero de sortie (`pcaChannel`) passe de la plage 0-15 a **0-31** :

```
Canal  0-15  →  Carte 1  (adresse I2C 0x40)
Canal 16-31  →  Carte 2  (adresse I2C 0x41)
```

Decomposition automatique dans le code :
```cpp
uint8_t board   = channel / 16;   // 0 ou 1
uint8_t pin     = channel % 16;   // 0-15
```

**L'utilisateur configure uniquement le numero de canal (0-31).** Pas de configuration
d'adresse I2C : les adresses 0x40 et 0x41 sont fixes dans le firmware.

La seconde carte est **detectee et initialisee automatiquement** si un canal >= 16
est present dans la configuration (doigts ou airflow).

---

## 2. Cas d'usage

| Instrument              | Trous | Canaux utilises     | Cartes PCA |
|-------------------------|-------|---------------------|------------|
| Flute irlandaise        | 6     | 0-5 + airflow       | 1          |
| Flute a bec baroque     | 8     | 0-7 + airflow       | 1          |
| Clarinette (17 cles)    | 17    | 0-16 + airflow      | 2          |
| Saxophone (23 cles)     | 23    | 0-22 + airflow      | 2          |

---

## 3. Retrocompatibilite

| Aspect                              | Impact                                      |
|--------------------------------------|----------------------------------------------|
| Config JSON existante (canaux 0-15)  | Aucun changement necessaire                  |
| Hardware avec 1 seule carte          | Fonctionne identiquement, carte 2 non initialisee |
| Presets existants (flutes, recorder) | Inchanges                                    |
| Interface web existante              | Fonctionnelle, limites etendues              |

---

## 4. Modifications par fichier

### 4.1 `settings.h`

Changements :
```cpp
// Avant
#define MAX_FINGER_SERVOS 15

// Apres
#define MAX_FINGER_SERVOS 31       // 2 cartes x 16 - 1 airflow
#define PCA_ADDR_BOARD0   0x40
#define PCA_ADDR_BOARD1   0x41
```

### 4.2 `InstrumentManager.h`

Remplacer l'instance unique `_pwm` par deux instances et une methode d'abstraction :

```cpp
private:
  Adafruit_PWMServoDriver _pwm0;     // Carte 1 (0x40) — toujours active
  Adafruit_PWMServoDriver _pwm1;     // Carte 2 (0x41) — active si canaux >= 16
  bool _secondBoardEnabled;

public:
  // Methode centralisee pour ecrire sur le bon PCA
  void setPWM(uint8_t channel, uint16_t on, uint16_t off);
```

### 4.3 `InstrumentManager.cpp`

```cpp
InstrumentManager::InstrumentManager()
  : _pwm0(PCA_ADDR_BOARD0),
    _pwm1(PCA_ADDR_BOARD1),
    _secondBoardEnabled(false),
    // ... reste inchange ...

void InstrumentManager::begin() {
  _pwm0.begin();
  _pwm0.setPWMFreq(SERVO_FREQUENCY);

  // Detection automatique de la seconde carte
  _secondBoardEnabled = false;
  for (int i = 0; i < cfg.numFingers; i++) {
    if (cfg.fingers[i].pcaChannel >= 16) {
      _secondBoardEnabled = true;
      break;
    }
  }
  if (cfg.airflowPcaChannel >= 16 || cfg.valveServoPcaChannel >= 16)
    _secondBoardEnabled = true;

  if (_secondBoardEnabled) {
    _pwm1.begin();
    _pwm1.setPWMFreq(SERVO_FREQUENCY);
    if (DEBUG) Serial.println("PCA9685 #2 (0x41) initialisee");
  }

  // ... reste inchange ...
}

void InstrumentManager::setPWM(uint8_t channel, uint16_t on, uint16_t off) {
  if (channel < 16) {
    _pwm0.setPWM(channel, on, off);
  } else {
    _pwm1.setPWM(channel - 16, on, off);
  }
}
```

### 4.4 `FingerController.h/.cpp`

Remplacer la reference directe au driver PCA par un callback :

```cpp
// Avant
FingerController(Adafruit_PWMServoDriver& pwm);
Adafruit_PWMServoDriver& _pwm;

// Apres
using PwmWriteFn = std::function<void(uint8_t channel, uint16_t on, uint16_t off)>;
FingerController(PwmWriteFn writePwm);
PwmWriteFn _writePwm;
```

Dans `setServoAngle()` :
```cpp
void FingerController::setServoAngle(int fingerIndex, uint16_t angle) {
  int pcaChannel = cfg.fingers[fingerIndex].pcaChannel;  // 0-31 maintenant
  uint16_t pwmValue = angleToPWM(angle);
  _writePwm(pcaChannel, 0, pwmValue);   // delegation au InstrumentManager
}
```

### 4.5 `AirflowController.h/.cpp`

Meme principe que FingerController :
- Remplacer `_pwm.setPWM(cfg.airflowPcaChannel, ...)` par `_writePwm(cfg.airflowPcaChannel, ...)`
- Idem pour `cfg.valveServoPcaChannel`
- Les valeurs de canal supportent deja 0-31 via le `uint8_t`

### 4.6 `Servo_flute_ESP32.ino` — `initSafeState()`

```cpp
void initSafeState() {
  Adafruit_PWMServoDriver pwm0(PCA_ADDR_BOARD0);
  pwm0.begin();
  pwm0.setPWMFreq(SERVO_FREQUENCY);
  // init servos canaux 0-15 comme actuellement

  // Seconde carte si necessaire
  bool needBoard1 = (DEFAULT_AIRFLOW_PCA_CHANNEL >= 16);
  for (int i = 0; i < NUM_DEFAULT_FINGERS && !needBoard1; i++) {
    if (DEFAULT_FINGERS[i].pcaChannel >= 16) needBoard1 = true;
  }
  if (needBoard1) {
    Adafruit_PWMServoDriver pwm1(PCA_ADDR_BOARD1);
    pwm1.begin();
    pwm1.setPWMFreq(SERVO_FREQUENCY);
    // init servos canaux 16-31
  }
}
```

### 4.7 `ConfigStorage.h`

Pas de modification structurelle. `FingerConfig.pcaChannel` reste un `uint8_t`,
sa plage passe simplement de 0-15 a 0-31.

Pas de nouveau champ de configuration necessaire : la detection est automatique.

### 4.8 `web_content.h` — Interface web

- Slider `numFingers` : augmenter la limite max de 15 a 31
- Champ canal PCA par doigt : accepter les valeurs 0-31
- Indication visuelle optionnelle pour distinguer carte 1 (canaux 0-15) vs carte 2 (canaux 16-31)

---

## 5. Considerations hardware

### Carte PCA9685 #2
- **Adresse I2C 0x41** : Souder le jumper **A0** sur la seconde carte
- Les deux cartes partagent le meme bus I2C (SDA=GPIO21, SCL=GPIO22) en daisy-chain

### Alimentation
- Chaque carte PCA9685 a son propre bornier V+ pour alimenter les servos
- Prevoir une alimentation 5V suffisante : chaque micro-servo consomme ~150-500mA en charge
- Avec 31 servos simultanement actifs : prevoir 5V / 8-10A minimum

### Bus I2C
- La capacitance du bus I2C augmente avec les cartes connectees
- Avec 2 cartes PCA9685 et des cables courts (<30cm), aucun probleme a 400kHz
- Si cables longs : ajouter des resistances pull-up de 2.2kΩ sur SDA et SCL

### Latence
- L'I2C a 400kHz permet ~60 ecritures PCA9685/ms
- Meme avec 31 servos a mettre a jour, la latence totale reste < 1ms
- Aucun impact perceptible sur le temps de reaction MIDI

---

## 6. Resume des fichiers a modifier

| Fichier                    | Nature du changement                          | Complexite |
|----------------------------|-----------------------------------------------|------------|
| `settings.h`              | Nouvelles constantes (MAX_FINGER_SERVOS, PCA) | Faible     |
| `InstrumentManager.h`     | 2 instances PWM + methode setPWM()            | Moyenne    |
| `InstrumentManager.cpp`   | Init multi-cartes + detection automatique     | Moyenne    |
| `FingerController.h/.cpp` | Callback PWM au lieu de reference directe     | Moyenne    |
| `AirflowController.h/.cpp`| Callback PWM au lieu de reference directe     | Moyenne    |
| `Servo_flute_ESP32.ino`   | initSafeState() multi-cartes                  | Faible     |
| `web_content.h`           | Limites etendues (canaux 0-31, doigts 1-31)   | Faible     |

---

## 7. Phases d'implementation recommandees

1. **Phase 1** — `InstrumentManager` : ajouter `_pwm0`/`_pwm1` et `setPWM()` virtuel
2. **Phase 2** — `FingerController` + `AirflowController` : refactorer avec callback PWM
3. **Phase 3** — `settings.h` + `initSafeState()` : constantes et init securisee
4. **Phase 4** — `web_content.h` : adapter les limites de l'interface
5. **Phase 5** — Presets : ajouter des doigtes pour instruments >15 trous
6. **Phase 6** — Tests avec 2 cartes physiques

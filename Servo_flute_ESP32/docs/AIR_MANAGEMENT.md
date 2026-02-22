# Gestion Air - Architecture Modulaire

## Vue d'ensemble

Le systeme de gestion d'air est modulaire et adaptatif. L'utilisateur choisit
un **type de systeme d'air** et l'interface web s'adapte automatiquement pour
n'afficher que les parametres pertinents.

## Types de systemes d'air supportes

### Mode 0 : Classique (Solenoide + Servo Flow)
Configuration de base. Une electrovalve solenoide coupe l'air, un servo
(servo flow) regle le debit.

```
[Solenoide] → [Servo Flow] → Flute
```

**Composants :** Solenoide GPIO, Servo airflow PCA9685
**Parametres :** GPIO pin solenoide, PWM activation/holding, canal PCA servo flow,
angles min/max/off

### Mode 1 : Servo-Valve (sans solenoide)
Une servo-valve PCA9685 remplace le solenoide pour couper l'air. Le servo flow
regle le debit.

```
[Servo-Valve PCA] → [Servo Flow] → Flute
```

**Composants :** Servo-valve PCA9685, Servo airflow PCA9685
**Parametres :** Canal PCA valve, angles ouvert/ferme, canal PCA servo flow

### Mode 2 : Servo Flow seul (sans valve)
Juste un servo flow qui regle le debit. Pas de coupure d'air, le servo se met
a l'angle "off" entre les notes.

```
[Servo Flow] → Flute
```

**Composants :** Servo airflow PCA9685
**Parametres :** Canal PCA servo flow, angles min/max/off

### Mode 3 : Ventilateur + Servo Flow
Un ventilateur (moteur DC/brushless) souffle en continu. Le servo flow dirige
le flux d'air vers le bec de la flute.

```
[Ventilateur PWM] → [Servo Flow] → Flute
```

**Composants :** Ventilateur GPIO PWM, Servo airflow PCA9685
**Parametres :** GPIO ventilateur, PWM min/max, canal PCA servo flow

### Mode 4 : Pompe(s) directe(s) + Valve + Servo Flow
Une ou plusieurs pompes (1 a 3) alimentent en air. Une valve coupe l'air entre
les notes.

```
[Pompe(s) PWM] → [Valve] → [Servo Flow] → Flute
```

**Composants :** 1-3 pompes GPIO PWM, Valve (solenoide ou servo), Servo airflow PCA
**Parametres :** Pour chaque pompe: GPIO, PWM min/max. Type valve, canal PCA servo flow

### Mode 5 : Pompe(s) + Reservoir (ballon) + Capteur distance
Les pompes remplissent un reservoir (ballon). Un capteur de distance ToF
(VL53L0X/VL6180X) mesure le niveau. Regulation par seuil/PID.

```
[Pompe(s) PWM] → [Reservoir + Capteur ToF] → [Valve] → [Servo Flow] → Flute
```

**Composants :** 1-3 pompes, Reservoir, Capteur ToF I2C, Valve, Servo airflow
**Parametres :** Pompes GPIO/PWM, type capteur, distances cible/min/max, PID Kp/Ki

### Mode 6 : Reservoir avec fin de course (pression/volume OK)
Reserve d'air avec un interrupteur de fin de course (endstop) qui indique
si la pression/volume du reservoir est suffisante. La pompe tourne jusqu'a
ce que le fin de course soit active.

```
[Pompe(s) PWM] → [Reservoir + Endstop] → [Valve] → [Servo Flow] → Flute
```

**Composants :** 1-3 pompes, Reservoir, Endstop GPIO, Valve, Servo airflow
**Parametres :** Pompes GPIO/PWM, GPIO endstop, logique active (HIGH/LOW)

## Composants optionnels communs

### Multi-pompes (1-3)
Tous les modes avec pompe supportent 1 a 3 pompes independantes. Chaque pompe
a son propre GPIO et ses parametres PWM min/max. Les pompes fonctionnent
en parallele.

### Type de valve
Quand une valve est presente (modes 0, 4, 5, 6) :
- **Solenoide GPIO** : electrovalve tout-ou-rien sur un GPIO
- **Servo PCA** : servo sur PCA9685 avec angle ouvert/ferme

### Servo Flow
Present dans tous les modes. Regle le debit d'air proportionnellement.

## Interface adaptive

L'onglet "Air" de l'interface web s'adapte dynamiquement au mode choisi :

| Section UI                | M0 | M1 | M2 | M3 | M4 | M5 | M6 |
|--------------------------|:--:|:--:|:--:|:--:|:--:|:--:|:--:|
| Selection mode           |  x |  x |  x |  x |  x |  x |  x |
| Servo Flow config        |  x |  x |  x |  x |  x |  x |  x |
| Solenoide config         |  x |    |    |    |  * |  * |  * |
| Servo-valve config       |    |  x |    |    |  * |  * |  * |
| Ventilateur config       |    |    |    |  x |    |    |    |
| Pompe(s) config          |    |    |    |    |  x |  x |  x |
| Nombre de pompes (1-3)   |    |    |    |    |  x |  x |  x |
| Reservoir + capteur ToF  |    |    |    |    |    |  x |    |
| Reservoir + endstop      |    |    |    |    |    |    |  x |
| Schema SVG adaptatif     |  x |  x |  x |  x |  x |  x |  x |
| Controle pompe live      |    |    |    |    |  x |  x |  x |
| Monitoring reservoir     |    |    |    |    |    |  x |  x |

`*` = sous-choix valve type (solenoide ou servo)

## Structure de donnees

```cpp
// settings.h - Modes
#define AIR_MODE_SOLENOID_SERVO   0  // Classique
#define AIR_MODE_SERVO_VALVE      1  // Servo-valve seule
#define AIR_MODE_SERVO_ONLY       2  // Servo flow seul
#define AIR_MODE_FAN_SERVO        3  // Ventilateur + servo flow
#define AIR_MODE_PUMP_VALVE       4  // Pompe(s) + valve
#define AIR_MODE_PUMP_RESERVOIR   5  // Pompe(s) + reservoir + capteur
#define AIR_MODE_PUMP_ENDSTOP     6  // Pompe(s) + reservoir + fin de course

// RuntimeConfig (ConfigStorage.h) - Champs air
struct RuntimeConfig {
  uint8_t airMode;              // 0-6
  // Valve
  uint8_t valveType;            // 0=solenoide GPIO, 1=servo PCA
  uint8_t valveServoPcaChannel;
  // Ventilateur
  uint8_t fanPin;
  uint8_t fanMinPwm;
  uint8_t fanMaxPwm;
  // Pompes (1-3)
  uint8_t numPumps;             // 1-3
  uint8_t pumpPins[3];
  uint8_t pumpMinPwm[3];
  uint8_t pumpMaxPwm[3];
  // Reservoir capteur ToF
  bool reservoirEnabled;
  uint8_t sensorType;           // 0=VL53L0X, 1=VL6180X
  uint16_t sensorTargetMm;
  uint16_t sensorMinMm;
  uint16_t sensorMaxMm;
  uint8_t pidKp;
  uint8_t pidKi;
  // Reservoir endstop
  uint8_t endstopPin;
  bool endstopActiveHigh;
  // UI
  bool showAirSystem;
};
```

---

# Roadmap

## Phase 1 - Architecture modulaire air (actuelle)
- [x] Definir les 7 modes de systeme d'air
- [x] Etendre RuntimeConfig avec tous les champs
- [x] Etendre ConfigStorage (load/save JSON)
- [x] Etendre l'API web (GET/POST /api/config)
- [x] Onglet Air adaptatif (affichage conditionnel par mode)
- [x] Support multi-pompes (1-3)
- [x] Support ventilateur (mode 3)
- [x] Support servo flow seul (mode 2)
- [x] Support fin de course reservoir (mode 6)
- [x] Schema SVG dynamique selon le mode
- [x] Wizard first-boot mis a jour avec tous les modes
- [x] Documentation AIR_MANAGEMENT.md

## Phase 2 - Hardware controllers (futur)
- [ ] FanController : gestion moteur ventilateur PWM
- [ ] Refactoring PressureController pour multi-pompe
- [ ] EndstopController : lecture fin de course avec debounce
- [ ] Integration AirflowController avec tous les modes

## Phase 3 - Optimisations (futur)
- [ ] Profils de pression par note (PID adaptatif)
- [ ] Courbe de reponse ventilateur
- [ ] Diagnostic automatique du systeme d'air
- [ ] Graphiques temps reel pression/debit dans l'UI

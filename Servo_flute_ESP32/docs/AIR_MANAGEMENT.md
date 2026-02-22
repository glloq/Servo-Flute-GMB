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
les notes. Les pompes peuvent etre PWM ou On/Off.

```
[Pompe(s)] → [Valve] → [Servo Flow] → Flute
```

**Composants :** 1-3 pompes GPIO (PWM ou On/Off), Valve (solenoide ou servo), Servo airflow PCA
**Parametres :** Type moteur (PWM/On/Off), pour chaque pompe: GPIO (+PWM min/max si PWM). Type valve, canal PCA servo flow

### Mode 5 : Pompe(s) + Reservoir + Capteur + Valve
Les pompes remplissent un reservoir (ballon). Un capteur mesure le niveau/pression.
Regulation par seuil/PID. Supporte 5 types de capteurs.

```
[Pompe(s)] → [Reservoir + Capteur] → [Valve] → [Servo Flow] → Flute
```

**Composants :** 1-3 pompes, Reservoir, Capteur (voir ci-dessous), Valve, Servo airflow
**Parametres :** Type moteur, pompes GPIO/PWM, type capteur + parametres specifiques, PID Kp/Ki

## Types de capteurs reservoir (mode 5)

| Type | Code | Description | Parametres |
|------|:----:|-------------|------------|
| VL53L0X | 0 | Capteur ToF I2C (laser) | Distance cible/min/max mm, PID Kp/Ki |
| VL6180X | 1 | Capteur ToF I2C (proche) | Distance cible/min/max mm, PID Kp/Ki |
| KY-024 Hall | 2 | Capteur effet Hall analogique | GPIO pin, seuil bas/haut, PID Kp/Ki |
| Endstop meca | 3 | Fin de course mecanique | GPIO pin, logique active HIGH/LOW |
| Endstop optique | 4 | Fin de course optique | GPIO pin, logique active HIGH/LOW |

## Types de moteur pompe (modes 4-5)

| Type | Code | Description |
|------|:----:|-------------|
| PWM variable | 0 | Controle vitesse via analogWrite, PWM min/max configurable |
| On/Off simple | 1 | Tout-ou-rien via digitalWrite, pas de controle de vitesse |

## Composants optionnels communs

### Multi-pompes (1-3)
Tous les modes avec pompe supportent 1 a 3 pompes independantes. Chaque pompe
a son propre GPIO et ses parametres PWM min/max (si moteur PWM). Les pompes
fonctionnent en parallele.

### Type de valve
Quand une valve est presente (modes 0, 4, 5) :
- **Solenoide GPIO** : electrovalve tout-ou-rien sur un GPIO
- **Servo PCA** : servo sur PCA9685 avec angle ouvert/ferme

### Servo Flow
Present dans tous les modes. Regle le debit d'air proportionnellement.

## Interface adaptive

L'onglet "Air" de l'interface web s'adapte dynamiquement au mode choisi :

| Section UI                | M0 | M1 | M2 | M3 | M4 | M5 |
|--------------------------|:--:|:--:|:--:|:--:|:--:|:--:|
| Selection mode           |  x |  x |  x |  x |  x |  x |
| Servo Flow config        |  x |  x |  x |  x |  x |  x |
| Solenoide config         |  x |    |    |    |  * |  * |
| Servo-valve config       |    |  x |    |    |  * |  * |
| Ventilateur config       |    |    |    |  x |    |    |
| Type moteur (PWM/OnOff)  |    |    |    |    |  x |  x |
| Pompe(s) config          |    |    |    |    |  x |  x |
| Nombre de pompes (1-3)   |    |    |    |    |  x |  x |
| Type capteur reservoir   |    |    |    |    |    |  x |
| Params capteur (ToF/Hall/Endstop) |  |  |  |  |  |  x |
| Schema SVG adaptatif     |  x |  x |  x |  x |  x |  x |
| Controle pompe live      |    |    |    |    |  x |  x |
| Monitoring reservoir     |    |    |    |    |    |  x |

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

// settings.h - Types moteur
#define MOTOR_TYPE_PWM    0  // PWM variable
#define MOTOR_TYPE_ONOFF  1  // On/Off simple

// settings.h - Types capteur
#define SENSOR_TYPE_TOF_VL53L0X    0  // ToF laser I2C
#define SENSOR_TYPE_TOF_VL6180X    1  // ToF proche I2C
#define SENSOR_TYPE_HALL_KY024     2  // Hall effet analogique
#define SENSOR_TYPE_ENDSTOP_MECH   3  // Fin de course mecanique
#define SENSOR_TYPE_ENDSTOP_OPT    4  // Fin de course optique

// RuntimeConfig (ConfigStorage.h)
struct RuntimeConfig {
  uint8_t airMode;              // 0-5
  uint8_t motorType;            // 0=PWM, 1=On/Off
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
  // Reservoir capteur
  uint8_t sensorType;           // 0-4 (SENSOR_TYPE_*)
  uint16_t sensorTargetMm;     // Cible ToF
  uint16_t sensorMinMm;        // Min ToF
  uint16_t sensorMaxMm;        // Max ToF
  uint8_t pidKp;
  uint8_t pidKi;
  // Hall (KY-024)
  uint8_t hallPin;              // GPIO analogique
  uint16_t hallThresholdLow;   // Seuil bas
  uint16_t hallThresholdHigh;  // Seuil haut
  // Endstop
  uint8_t endstopPin;
  bool endstopActiveHigh;
  // UI
  bool showAirSystem;
};
```

## Schema SVG

Le schema SVG dans l'onglet Air se genere dynamiquement selon le mode :

```
Layout (modes 4-5 avec reservoir) :

         [Servo Flow] ──→ [Flute]
              ↑
[Pompe(s)] ──→ [Reservoir] ──→ [Valve]

Layout (modes simples) :

         [Servo Flow] ──→ [Flute]
              ↑
[Source] ──→ [Valve]
```

- L'air arrive par la GAUCHE (source = pompe/ventilateur/fleche)
- Passe optionnellement par un reservoir
- Entre dans la valve au centre-bas
- La sortie de valve monte VERTICALEMENT
- Le servo flow est AU-DESSUS de la valve
- La sortie va a DROITE vers l'embouchure de la flute

---

# Roadmap

## Phase 1 - Architecture modulaire air (completee)
- [x] Definir les 6 modes de systeme d'air
- [x] Support moteur PWM et On/Off
- [x] Support 5 types de capteurs reservoir
- [x] Etendre RuntimeConfig avec tous les champs
- [x] Etendre ConfigStorage (load/save JSON)
- [x] Retro-compatibilite ancien mode 6 -> mode 5 + endstop
- [x] Etendre l'API web (GET/POST /api/config)
- [x] Onglet Air adaptatif (affichage conditionnel par mode + capteur + moteur)
- [x] Support multi-pompes (1-3)
- [x] Support ventilateur (mode 3)
- [x] Support servo flow seul (mode 2)
- [x] Schema SVG dynamique selon le mode
- [x] WebSocket live stats (distance, Hall, endstop)
- [x] Wizard first-boot mis a jour avec tous les modes
- [x] Documentation AIR_MANAGEMENT.md

## Phase 2 - Hardware controllers (completee)
- [x] FanController : gestion moteur ventilateur PWM (rampe douce 300ms, mapping percent→PWM)
- [x] Refactoring PressureController pour multi-pompe (PID remplace hysteresis, per-pump PWM scaling)
- [x] Integration AirflowController avec tous les modes (fan mode 3, servo-only mode 2)
- [x] InstrumentManager : orchestration FanController (init, update, allSoundOff)
- [x] NoteSequencer : conscience des modes (valve virtuelle modes 2-3)
- [x] WebConfigurator : broadcast fan_pwm/fan_speed/fan_ready, commandes WS fan_target/fan_stop

## Phase 3 - Optimisations (futur)
- [ ] Profils de pression par note (PID adaptatif)
- [ ] Courbe de reponse ventilateur
- [ ] Diagnostic automatique du systeme d'air
- [ ] Graphiques temps reel pression/debit dans l'UI

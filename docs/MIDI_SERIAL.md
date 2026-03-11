# MIDI Serial (DIN)

Reception MIDI depuis un module hardware (clavier, sequenceur, pedalier...) via une prise DIN 5 broches standard.

Le MIDI Serial fonctionne en parallele avec le BLE-MIDI et le rtpMIDI WiFi. Il se configure depuis l'interface web : **Reglages > MIDI Serial (DIN)**.

## Composants necessaires

| Composant | Reference | Quantite | Role |
|-----------|-----------|----------|------|
| Optocoupler | 6N138 (ou H11L1) | 1 | Isolation galvanique (norme MIDI) |
| Diode | 1N4148 | 1 | Protection contre inversion de polarite |
| Resistance | 220 ohm | 1 | Limitation courant LED de l'optocoupler |
| Resistance | 470 ohm | 1 | Pull-up sortie optocoupler |
| Connecteur | DIN-5 femelle (chassis) | 1 | Prise MIDI IN standard |

> Cout total : ~2-3 EUR en composants discrets.

## Schema de cablage

```
                   MIDI IN (DIN-5 femelle, vue arriere)
                        ┌───────────┐
                        │  5     4  │
                        │     2     │
                        │  1     3  │
                        └───────────┘
                          │     │
                          │     └──────────── GND
                          │
                       ┌──┴──┐
                       │220 R│
                       └──┬──┘
                          │
              ┌───────────┴───────────────┐
              │      6N138 (vue dessus)   │
              │                           │
    Pin 2 ────┤ Anode           Vcc ├──── 3.3V  (pin 8)
              │       ┌─1N4148─┐          │
    Pin 3 ────┤ Cath  │K     A│    GND ├──── GND   (pin 5)
              │       └────────┘          │
              │                    Out ├──┬── GPIO ESP32 (RX)
              │                           │  │
              └───────────────────────────┘  │
                                          ┌──┴──┐
                                          │470 R│
                                          └──┬──┘
                                             │
                                           3.3V
```

**Connexions du 6N138 :**

| Pin 6N138 | Connexion |
|-----------|-----------|
| Pin 2 (Anode) | DIN-5 pin 5 via 220 ohm |
| Pin 3 (Cathode) | DIN-5 pin 4 (+ 1N4148 cathode vers pin 2) |
| Pin 5 (GND) | GND ESP32 |
| Pin 6 (Output) | GPIO ESP32 + pull-up 470 ohm vers 3.3V |
| Pin 8 (Vcc) | 3.3V ESP32 |

## GPIO compatible

Le GPIO RX est selectionnable dans **Reglages > MIDI Serial (DIN) > Pin RX**.

### Pins disponibles

| GPIO | Remarque |
|------|----------|
| **16** | RX2 par defaut, recommande |
| 17 | TX2 |
| 18 | VSPI CLK |
| 19 | VSPI MISO |
| 23 | VSPI MOSI |
| 25 | DAC1 |
| 26 | DAC2 |
| 27 | — |
| 33 | ADC1 |
| **34** | Entree uniquement — ideal pour RX |
| **35** | Entree uniquement — ideal pour RX |
| **36** | Entree uniquement — ideal pour RX |
| **39** | Entree uniquement — ideal pour RX |

> Les pins 34-39 sont en entree uniquement et conviennent parfaitement pour la reception MIDI.

### Pins reservees (non disponibles)

| GPIO | Utilisation |
|------|-------------|
| 0 | Bouton BOOT |
| 2 | LED d'etat |
| 4 | Switch BT/WiFi |
| 5 | PCA9685 OE |
| 6-11 | Flash SPI interne |
| 12 | Strapping pin (boot) |
| 13 | Solenoide (PWM) |
| 14 | INMP441 BCLK |
| 15 | INMP441 LRCLK |
| 21 | I2C SDA (PCA9685) |
| 22 | I2C SCL (PCA9685) |
| 32 | INMP441 DIN |

## Points importants

- **Alimentation 3.3V** : Le 6N138 doit etre alimente en 3.3V (pas 5V). Les GPIO ESP32 ne tolerent pas le 5V.
- **Redemarrage requis** : Un changement de GPIO necessite un redemarrage de l'ESP32 pour prendre effet.
- **Baud rate** : Le MIDI Serial utilise 31250 baud (standard MIDI), configure automatiquement par le firmware.
- **MIDI IN uniquement** : Ce circuit est pour la reception. L'envoi MIDI (MIDI OUT/THRU) n'est pas supporte.

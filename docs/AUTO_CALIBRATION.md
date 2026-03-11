# Auto-calibration avec micro INMP441

Module optionnel de calibration automatique utilisant un microphone MEMS I2S pour detecter le son produit par l'instrument.

Pour la calibration manuelle (sliders, assistant doigts, tests), voir [CALIBRATION.md](CALIBRATION.md).

## Materiel necessaire

| Pin INMP441 | GPIO ESP32 | Description |
|-------------|------------|-------------|
| SCK (BCLK) | GPIO 14 | Horloge bit I2S |
| WS (LRCLK) | GPIO 15 | Selection canal I2S |
| SD (DIN) | GPIO 32 | Donnees audio I2S |
| VDD | 3.3V | Alimentation |
| GND | GND | Masse |
| L/R | GND | Canal gauche (connecter a GND) |

## Detection automatique

Au demarrage, le firmware tente de lire des echantillons I2S. Si >10% des samples sont non-nuls, le micro est considere comme present. Le champ `"mic": true` apparait dans la reponse de `/api/config`, et la section auto-calibration est affichee dans l'interface web.

Si le micro n'est pas detecte, le driver I2S est desinstalle pour liberer les ressources. L'interface web fonctionne normalement sans la section auto-calibration.

## Utilisation

1. Aller dans l'onglet **Calibration**
2. La section **Auto-calibration** apparait en bas (si micro detecte)
3. Le **VU-metre** et l'**affichage du pitch** montrent en temps reel ce que le micro capte

### Range Finder (detection plage servo)

Avant la calibration par note, le **Range Finder** permet de detecter automatiquement la plage utile du servo airflow :

1. Cliquer **Detecter plage servo**
2. Le systeme positionne les doigts pour une note du milieu de la tessiture
3. Le servo airflow balaye de 0 a 180 degres (a 100ms/pas)
4. Le micro detecte l'angle minimum (debut du son) et maximum (fin du son)
5. Les resultats s'affichent avec les boutons **Appliquer** / **Ignorer**
6. "Appliquer" met a jour `air_min` et `air_max` dans la configuration

### Auto-calibration airflow par note

1. Cliquer **Demarrer auto-calibration** :
   - Pour chaque note, le systeme positionne les doigts et ouvre le solenoide
   - Le debit d'air est augmente progressivement du minimum au maximum
   - Le micro detecte le debut du son (air_min) et la fin du son (air_max)
   - La hauteur du son est verifiee pour s'assurer que c'est bien la bonne note (tolerance ±3 demi-tons)
   - L'angle servo courant est affiche en temps reel pendant le sweep
2. Les resultats sont affiches note par note avec pourcentages et angles en degres
3. Les valeurs sont automatiquement sauvegardees dans la configuration

## Parametres

Les constantes de l'auto-calibration sont definies dans `settings.h` :

| Constante | Defaut | Description |
|-----------|--------|-------------|
| `AUTOCAL_SETTLE_MS` | 300 | Temps de stabilisation des servos (ms) |
| `AUTOCAL_STEP_MS` | 80 | Intervalle entre chaque pas de sweep (ms) |
| `AUTOCAL_SILENCE_COUNT` | 3 | Lectures silencieuses consecutives pour valider air_max |
| `AUTOCAL_RF_STEP_MS` | 100 | Intervalle entre pas du range finder (ms) |
| `AUTOCAL_RF_MARGIN_DEG` | 3 | Marge de securite en degres pour le range finder |
| `MIC_RMS_THRESHOLD` | 0.02 | Seuil RMS pour la detection de son |
| `MIC_PITCH_TOLERANCE_CENTS` | 200 | Tolerance de pitch en cents (±2 demi-tons) |
| `MIC_YIN_THRESHOLD` | 0.15 | Seuil de confiance pour la detection de pitch |

## Fonctionnement du sweep

Pour chaque note :
1. **Preparation** : positionnement des doigts + ouverture solenoide
2. **Stabilisation** : attente 300ms pour que les servos se stabilisent
3. **Sweep** : l'angle airflow augmente progressivement de `air_off` vers `air_max`
   - Detection du **debut du son** : RMS > seuil ET pitch dans la tolerance → **air_min**
   - Detection de la **fin du son** : 3 lectures silencieuses consecutives apres le son → **air_max**
4. **Resultat** : air_min et air_max sont stockes en pourcentage (0-100%)

## Commandes WebSocket

| Commande | JSON | Description |
|----------|------|-------------|
| Monitoring micro | `{"t":"mic_mon","on":true}` | Active/desactive le flux audio temps reel |
| Auto-calibration | `{"t":"auto_cal","mode":"air"}` | Demarre la calibration automatique airflow |
| Range finder | `{"t":"auto_cal","mode":"range"}` | Detecte la plage servo airflow (sweep 0-180) |
| Appliquer range | `{"t":"auto_cal","mode":"apply_range"}` | Applique les resultats du range finder |
| Stop auto-cal | `{"t":"auto_cal","mode":"stop"}` | Arrete la calibration en cours |

Pour le detail des messages serveur (progression, resultats), voir [API_WEB.md](API_WEB.md).

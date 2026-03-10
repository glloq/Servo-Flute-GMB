# Calibration

## Premiere utilisation

### 1. Preparation mecanique

Avant de fixer les doigts sur les servos :
- Les servos doivent etre alimentes et initialises (angles fermes definis dans `settings.h`)
- Le firmware doit etre flashe sur l'ESP32
- Se connecter au hotspot `ServoFlute-Setup` (pas de mot de passe)
- Ouvrir `192.168.4.1` dans un navigateur

### 2. Calibration des doigts

Aller dans l'onglet **Calibration** :

1. **Slider par doigt** : chaque slider controle un servo en temps reel (0-180 deg)
2. Fixer le doigt sur le servo dans la position souhaitee pour "ferme"
3. Verifier que l'angle ferme est correct
4. Verifier que l'ouverture (ferme + angle_open * direction) degage bien le trou
5. Ajuster dans l'onglet **Config** > section Doigts si necessaire

### 3. Calibration airflow

1. **Slider airflow** : controle le servo de debit en temps reel
2. Trouver l'angle repos (pas de son) -> `air_off`
3. Trouver l'angle minimum (son le plus faible) -> `air_min`
4. Trouver l'angle maximum (son le plus fort) -> `air_max`
5. Reporter les valeurs dans Config > Airflow

### 4. Test solenoide

1. Bouton **OUVRIR** : active la valve
2. Bouton **FERMER** : desactive la valve
3. Verifier que le debit d'air passe bien quand le solenoide est ouvert

### 5. Test par note

1. Selectionner une note dans le dropdown
2. Cliquer **Jouer position** : positionne les doigts + airflow pour cette note (sans solenoide)
3. Verifier visuellement que le bon pattern de doigts est applique
4. Si le son est trop fort/faible pour certaines notes, ajuster les pourcentages airflow dans Config > Airflow par note

### 6. Sauvegarder

Aller dans **Config** et cliquer **Sauvegarder**. Les valeurs sont persistees sur LittleFS.

## Auto-calibration avec micro INMP441 (optionnel)

Si un microphone INMP441 est connecte (voir brochage ci-dessous), l'interface de calibration affiche automatiquement une section supplementaire **Auto-calibration** avec VU-metre et detection de pitch en temps reel.

### Materiel necessaire

| Pin INMP441 | GPIO ESP32 | Description |
|-------------|------------|-------------|
| SCK (BCLK) | GPIO 14 | Horloge bit I2S |
| WS (LRCLK) | GPIO 15 | Selection canal I2S |
| SD (DIN) | GPIO 32 | Donnees audio I2S |
| VDD | 3.3V | Alimentation |
| GND | GND | Masse |
| L/R | GND | Canal gauche (connecter a GND) |

### Detection automatique

Au demarrage, le firmware tente de lire des echantillons I2S. Si >10% des samples sont non-nuls, le micro est considere comme present. Le champ `"mic": true` apparait dans la reponse de `/api/config`, et la section auto-calibration est affichee dans l'interface web.

Si le micro n'est pas detecte, le driver I2S est desinstalle pour liberer les ressources. L'interface web fonctionne normalement sans la section auto-calibration.

### Utilisation

1. Aller dans l'onglet **Calibration**
2. La section **Auto-calibration** apparait en bas (si micro detecte)
3. Le **VU-metre** et l'**affichage du pitch** montrent en temps reel ce que le micro capte
4. Cliquer **Demarrer auto-calibration** :
   - Pour chaque note, le systeme positionne les doigts et ouvre le solenoide
   - Le debit d'air est augmente progressivement du minimum au maximum
   - Le micro detecte le debut du son (air_min) et la fin du son (air_max)
   - La hauteur du son est verifiee pour s'assurer que c'est bien la bonne note (tolerance ±3 demi-tons)
5. Les resultats sont affiches note par note (reussite/echec avec pourcentages)
6. Les valeurs sont automatiquement sauvegardees dans la configuration

### Parametres de calibration

Les constantes de l'auto-calibration sont definies dans `settings.h` :

| Constante | Defaut | Description |
|-----------|--------|-------------|
| `AUTOCAL_SETTLE_MS` | 300 | Temps de stabilisation des servos (ms) |
| `AUTOCAL_STEP_MS` | 80 | Intervalle entre chaque pas de sweep (ms) |
| `AUTOCAL_SILENCE_COUNT` | 3 | Lectures silencieuses consecutives pour valider air_max |
| `MIC_RMS_THRESHOLD` | 0.02 | Seuil RMS pour la detection de son |
| `MIC_PITCH_TOLERANCE_CENTS` | 200 | Tolerance de pitch en cents (±2 demi-tons) |
| `MIC_YIN_THRESHOLD` | 0.15 | Seuil de confiance pour la detection de pitch |

### Fonctionnement du sweep

Pour chaque note :
1. **Preparation** : positionnement des doigts + ouverture solenoide
2. **Stabilisation** : attente 300ms pour que les servos se stabilisent
3. **Sweep** : l'angle airflow augmente progressivement de `air_off` vers `air_max`
   - Detection du **debut du son** : RMS > seuil ET pitch dans la tolerance → **air_min**
   - Detection de la **fin du son** : 3 lectures silencieuses consecutives apres le son → **air_max**
4. **Resultat** : air_min et air_max sont stockes en pourcentage (0-100%)

## Interface de calibration

### Assistant calibration doigts

L'assistant guide pas a pas pour calibrer l'angle de fermeture de chaque doigt :
1. Pour chaque doigt, un slider temps reel permet de trouver l'angle de fermeture exact
2. A la fin, une etape de validation permet de tester ouverture/fermeture et d'inverser la direction
3. Les valeurs sont sauvegardees automatiquement

### Calibration par note

Pour chaque note individuellement :
1. Selectionner une note dans la liste
2. Le panneau contextuel affiche les sliders air_min/air_max avec preview en temps reel
3. L'outil **Sweep souffle** balaye automatiquement le debit d'air pendant que l'utilisateur marque le debut et la fin du son
4. Representation visuelle de la flute avec les doigtes en temps reel

### Table des doigtes visuelle

L'onglet **Config** > section **Doigtes et airflow par note** affiche un tableau interactif :
- Points cliquables pour chaque doigt (ouvert/ferme)
- Valeurs air min/max editables
- Bouton test par note

### Commandes WebSocket

Depuis l'interface web, les commandes de calibration passent par WebSocket pour une reponse temps reel :

| Commande | JSON | Description |
|----------|------|-------------|
| Test doigt | `{"t":"test_finger","i":0,"a":90}` | Position le doigt `i` a l'angle `a` |
| Test airflow | `{"t":"test_air","a":60}` | Position le servo airflow a l'angle `a` |
| Test solenoide | `{"t":"test_sol","o":1}` | Ouvre (1) ou ferme (0) le solenoide |
| Test note | `{"t":"test_note","n":84}` | Applique doigts + airflow pour la note MIDI `n` |
| Monitoring micro | `{"t":"mic_mon","on":true}` | Active/desactive le flux audio temps reel |
| Auto-calibration | `{"t":"auto_cal","mode":"air"}` | Demarre la calibration automatique airflow |
| Stop auto-cal | `{"t":"auto_cal","mode":"stop"}` | Arrete la calibration en cours |

### Securite

- Le bouton **TOUT ARRETER** envoie un `panic` + remet l'airflow au repos + ferme le solenoide
- Pas de timeout automatique sur les commandes de test (le servo reste en position)
- Le watchdog ESP32 (4s) protege contre les blocages systeme

## Maintenance

Pour recalibrer apres changement de servo ou de mecanique :
1. Onglet Calibration : tester les angles avec les sliders
2. Onglet Config : reporter les nouvelles valeurs
3. Sauvegarder

Pour repartir de zero :
1. Onglet Config : **Reset defauts**
2. Les valeurs de `settings.h` sont restaurees

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

### 5. Reglage demi-ouverture

Pour les instruments utilisant des demi-trous (flute a bec baroque, etc.) :

1. Le slider **Demi-ouverture** (global, 10-90%) definit le pourcentage d'ouverture par defaut pour les positions "demi" (valeur 2 dans le pattern de doigtes)
2. Chaque doigt peut avoir un **override individuel** via le slider "Demi %" dans sa carte (Step 1). Mettre 0 = utiliser le global
3. Le bouton **Demi** dans chaque carte permet de tester la position demi-ouverte du servo en temps reel
4. Cas typique : sur une flute a bec, le pouce (doigt 1) necessite un % different (~25%) des trous avant (~50%)

### 6. Test par note

1. Selectionner une note dans le dropdown
2. Cliquer **Jouer position** : positionne les doigts + airflow pour cette note (sans solenoide)
3. Verifier visuellement que le bon pattern de doigts est applique
4. Si le son est trop fort/faible pour certaines notes, ajuster les pourcentages airflow dans Config > Airflow par note

### 7. Sauvegarder

Aller dans **Config** et cliquer **Sauvegarder**. Les valeurs sont persistees sur LittleFS.

## Auto-calibration (optionnel)

Avec un micro INMP441, le systeme peut calibrer automatiquement les plages d'airflow. Voir [AUTO_CALIBRATION.md](AUTO_CALIBRATION.md).

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
- Points cliquables pour chaque doigt, cycle 3 etats : ferme (0) → ouvert (1) → demi-ouvert (2)
- Legende visuelle : ouvert (vert), ferme (gris), demi (degrade), pouce (bordure rouge)
- Valeurs air min/max editables avec affichage en degres
- Bouton test par note

### Commandes WebSocket

Depuis l'interface web, les commandes de calibration passent par WebSocket pour une reponse temps reel :

| Commande | JSON | Description |
|----------|------|-------------|
| Test doigt | `{"t":"test_finger","i":0,"a":90}` | Position le doigt `i` a l'angle `a` |
| Test airflow | `{"t":"test_air","a":60}` | Position le servo airflow a l'angle `a` |
| Test solenoide | `{"t":"test_sol","o":1}` | Ouvre (1) ou ferme (0) le solenoide |
| Test note | `{"t":"test_note","n":84}` | Applique doigts + airflow pour la note MIDI `n` |

Pour les commandes auto-calibration (micro), voir [AUTO_CALIBRATION.md](AUTO_CALIBRATION.md#commandes-websocket).

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

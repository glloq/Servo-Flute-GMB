# Servo Angle — Montage sur flute traversiere

Guide de montage et de reglage des servos pour les flutes tenues horizontalement (traversiere, bansuri, dizi, fifre).

## Principe de fonctionnement

Chaque servo doigt est defini par 3 parametres :

| Parametre | Description | Reglage |
|-----------|-------------|---------|
| **Angle ferme** (`closedAngle`) | Position du servo quand le doigt bouche le trou (0-180°) | Par doigt, via calibration |
| **Angle d'ouverture** (`angle_open`) | Amplitude du mouvement d'ouverture (10-90°) | Global, via Reglages > Doigts |
| **Direction** (`direction`) | Sens de rotation : +1 ou -1 | Par doigt, via calibration |

Le calcul est simple :
```
Position ouverte = closedAngle + (angle_open × direction)
Position demi   = closedAngle + (angle_open × direction × halfPercent / 100)
```

## Difference flute a bec vs traversiere

### Flute a bec (verticale)

```
      Embouchure
         │
    ┌────┴────┐
    │  Trou 1 │ ← Servo monte perpendiculaire au corps
    │  Trou 2 │    Le bras du servo fait ~30° de rotation
    │  Trou 3 │    pour degager le trou
    │  Trou 4 │
    │  Trou 5 │
    │  Trou 6 │
    └─────────┘
```

Les servos sont montes **autour du corps** de la flute. Le bras vient se plaquer sur le trou par-dessus.

### Flute traversiere (horizontale)

```
    Embouchure
         │
    ┌────┴──────────────────────────────┐
    │    ◉      ◉      ◉      ◉      ◉│  ← Trous sur le dessus
    └───────────────────────────────────┘
         │      │      │      │      │
       Servo  Servo  Servo  Servo  Servo
```

Les servos sont montes **en dessous** du corps de la flute. Le bras passe par-dessus pour atteindre le trou.

**Consequence** : la direction de rotation est generalement **inversee** par rapport a une flute a bec. C'est pourquoi le parametre `direction` existe.

## Reglages recommandes

### Angle d'ouverture

| Type d'instrument | `angle_open` recommande | Notes |
|-------------------|------------------------|-------|
| Flute a bec | 25-35° | Trous petits, peu de debattement |
| Tin whistle | 25-30° | Similaire a la flute a bec |
| Flute traversiere | 30-45° | Trous plus grands, servo plus eloigne |
| Bansuri | 35-50° | Grands trous, bras de levier long |
| Fifre | 25-35° | Petit instrument, trous rapproches |

> **Regle** : plus le servo est eloigne du trou, plus l'angle d'ouverture doit etre grand pour degager completement le trou.

### Preset "Puissant" pour l'air

Les flutes traversieres demandent generalement plus de souffle. Dans l'onglet Air, le preset **Puissant** (air_min=15°, air_max=120°) est adapte.

## Etapes de calibration

### 1. Choisir le preset instrument

Dans le **wizard de premiere utilisation** ou dans **Calibration > Presets** :
- Flute irlandaise (Do) — `em: trav`
- Bansuri (La) — `em: trav`
- Dizi (Re) — `em: trav`
- Fifre (Sib) — `em: trav`

Le preset configure automatiquement l'embouchure `trav`, les doigtes et les plages d'airflow adaptees.

### 2. Fixer les servos mecaniquement

1. Alimenter les servos (ils se placent a leur angle ferme par defaut : ~90°)
2. Fixer chaque servo sous la flute, bras orienté vers le trou correspondant
3. **Ne pas visser le bras** avant la calibration

### 3. Calibrer les angles fermes

Aller dans **Calibration > Assistant doigts** :

1. Pour chaque doigt, utiliser le slider pour trouver l'angle exact ou le bras bouche le trou
2. Visser le bras du servo dans cette position
3. Passer au doigt suivant

### 4. Regler la direction

Toujours dans l'assistant :

1. Cliquer **Ouvrir** pour chaque doigt
2. Si le servo tourne dans le mauvais sens (enfonce au lieu d'ouvrir), inverser la direction avec le bouton **Inverser**
3. Verifier que **Ouvrir** degage bien le trou et que **Fermer** le rebouche

### 5. Ajuster l'angle d'ouverture global

Dans **Calibration > Doigts** :

1. Le slider **Angle d'ouverture** controle le debattement global (defaut : 30°)
2. Augmenter si les doigts n'ouvrent pas assez (trou pas totalement degage)
3. Diminuer si les doigts vont trop loin (collision avec la mecanique)
4. Tester avec **Ouvrir/Fermer** sur chaque doigt

### 6. Demi-ouverture (optionnel)

Pour les instruments utilisant des demi-trous (trilles, ornements) :

1. Le slider **Demi-ouverture** (global, 10-90%) definit le pourcentage par defaut
2. Chaque doigt peut avoir un override individuel via son slider **Demi %**
3. Typique pour traversiere : 40-60% selon la taille des trous

## Montage mecanique — conseils

### Fixation des servos

- Utiliser de la colle chaude ou des supports imprimes 3D
- Les servos SG90 a engrenages metalliques sont recommandes (plus precis, plus durables)
- Prevoir un jeu de ~1mm entre le bras et le trou pour eviter les frottements

### Bras de servo (palonniers)

- Utiliser le bras **simple** (pas en croix) pour un meilleur controle
- La longueur du bras influence l'amplitude : bras court = mouvement precis, bras long = plus de portee
- Pour les grands trous (bansuri), un bras rallonge avec un tampon en mousse peut etre necessaire

### Gestion des cables

- Regrouper les cables servo avec de la gaine thermoretractable
- Prevoir assez de longueur pour ne pas tirer sur les connecteurs PCA9685
- Numerotation : marquer chaque cable avec le numero de canal PCA

## Depannage

| Probleme | Cause probable | Solution |
|----------|---------------|----------|
| Le doigt ne bouche pas completement | Angle ferme mal regle | Recalibrer via l'assistant |
| Le doigt n'ouvre pas assez | `angle_open` trop faible | Augmenter dans Doigts > Angle d'ouverture |
| Le doigt va trop loin | `angle_open` trop grand | Diminuer ou ajuster le bras servo |
| Le servo vibre/grince | Pression mecanique | Verifier qu'il n'y a pas de friction |
| Mouvement inverse | Mauvaise direction | Inverser dans l'assistant calibration |
| Son fausse sur certaines notes | Trou pas assez bouche | Ajouter un tampon souple sur le bras |

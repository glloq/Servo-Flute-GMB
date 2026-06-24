# Servo Angle — Mounting on Transverse Flutes

Mounting and calibration guide for flutes held horizontally, such as transverse flute, bansuri, dizi, and fife.

## Operating principle

Each finger servo is configured with:

| Parameter | Description | Where to set it |
|-----------|-------------|-----------------|
| **Closed angle** (`closedAngle`) | Servo position when the finger closes the hole (0-180°) | Per finger, through calibration |
| **Opening angle** (`angle_open`) | Opening travel amplitude (10-90°) | Global, Settings > Fingers |
| **Direction** (`direction`) | Rotation direction: +1 or -1 | Per finger, through calibration |

```
open_position = closedAngle + (angle_open * direction)
half_position = closedAngle + (angle_open * direction * halfPercent / 100)
```

## Recorder vs transverse flute

On a recorder, servos are usually mounted around the body and press directly onto the holes. On a transverse flute, servos are usually mounted below the body and the arm reaches over the tube to close the hole. This often reverses the required servo direction, which is why `direction` is configurable per finger.

## Recommended opening angles

| Instrument | Recommended `angle_open` | Notes |
|------------|--------------------------|-------|
| Recorder | 25-35° | Small holes, short travel |
| Tin whistle | 25-30° | Similar to recorder |
| Transverse flute | 30-45° | Larger holes, servo farther away |
| Bansuri | 35-50° | Large holes, longer lever arm |
| Fife | 25-35° | Small instrument, close holes |

Rule of thumb: the farther the servo is from the hole, the larger the opening angle must be to clear the hole fully.

## Air preset

Transverse flutes usually need more breath. In the Air tab, the **Powerful** preset (`air_min=15°`, `air_max=120°`) is a good starting point.

## Calibration steps

1. Select a transverse-flute preset in the first-use wizard or Calibration presets.
2. Power the servos so they move to their default closed angle.
3. Mount each servo below the flute with its arm pointing toward the matching hole.
4. Do not screw the servo arm down until the closed angle is calibrated.
5. In Calibration > Fingers, use the slider to find the exact closed angle for each finger.
6. Test Open/Close. If a servo moves the wrong way, invert its direction.
7. Adjust the global opening angle if the holes do not open enough or if the mechanism collides.
8. Configure half-open percentages for ornaments or half-hole fingerings when needed.

## Mechanical advice

- Use hot glue or 3D-printed brackets for prototypes.
- Metal-gear SG90 servos are recommended for precision and durability.
- Leave about 1 mm of clearance between the arm and the hole to avoid rubbing.
- Prefer a single straight horn instead of a cross horn.
- For large holes, add a foam pad to the arm.
- Label each servo cable with its PCA9685 channel.

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|--------------|-----|
| Finger does not close fully | Closed angle is wrong | Recalibrate the closed angle |
| Finger does not open enough | `angle_open` is too low | Increase the opening angle |
| Movement is reversed | Direction is wrong | Invert direction in calibration |
| Pitch is wrong on some notes | Hole is not sealed | Add a soft pad or recalibrate |

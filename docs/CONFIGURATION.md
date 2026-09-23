# Configuration

Configuration is stored in `RuntimeConfig` and persisted as JSON on LittleFS. Defaults are defined in `Servo_flute_ESP32/settings.h`.

## Instrument

- number of fingers;
- number of notes;
- PCA channel per finger;
- closed angle and direction per finger;
- half-hole percentage;
- mouthpiece type;
- note table and fingering patterns.

## Airflow

- air mode;
- flow servo PCA channel;
- per-note airflow min/max **and recommended nominal** (`airflowNominalPercent`,
  JSON key `anm`), constrained to `0 ≤ min ≤ nominal ≤ max ≤ 100`;
- optional angle servo for transverse flutes;
- solenoid or servo-valve settings;
- pump, fan, reservoir, and sensor settings.

### Per-note nominal airflow (backward-compatible)

Each note carries `airflowNominalPercent`, the recommended airflow within its
min/max window, produced by microphone auto-calibration or edited manually.

- Persisted as JSON key `anm` in each `notes[]` entry, alongside `amn`/`amx`.
- **Migration:** an old config file (or an old web client) that omits `anm` is
  loaded transparently — the nominal is derived as
  `min + 0.40·(max − min)`.
- The central `RuntimeConfig` validator rejects any note where
  `nominal < min` or `nominal > max`.

## Microphone, acoustic analysis and auto-calibration constants

### Where a parameter lives, and who may touch it

The acoustic parameters are **deliberately not all in `settings.h`**, and the
split tells you who owns what:

| Family | Where | What it is |
|---|---|---|
| **Firmware** | `settings.h`, 44 `MIC_*` macros | the hardware and the processing chain: sample rate, frame and hop size, DMA, filter cutoffs, FFT size and decimation. Changing one changes *the measurement itself*. |
| **Instrument** | `AcousticQuality.h`, 34 `AQ_*` constants | the thresholds that decide whether a note is good, breathy, unstable, overblown. Each one is **calibrated**, and carries above it the measurement that justifies it. |
| **Learned** | runtime only | the seven per-machine-state noise profiles. **Lost on reboot** — persistence does not exist yet. |

The `MIC_*` macros group by role: acquisition and analysis rate
(`MIC_SAMPLE_RATE`, `MIC_BUFFER_SIZE`, `MIC_ANALYSIS_FRAME_SIZE`,
`MIC_ANALYSIS_HOP_SIZE`, `MIC_RING_CAPACITY`, `MIC_DRAIN_INTERVAL_MS`,
`MIC_FRAME_STALE_MS`); pitch detection (`MIC_PITCH_MIN_HZ`/`MAX_HZ`,
`MIC_YIN_*`, `MIC_PITCH_HISTORY`); spectral analysis (`MIC_FFT_ENABLED`,
`MIC_FFT_SIZE`, `MIC_SPECTRAL_DECIMATION`, `MIC_HNR_MAX_DB`); stream filtering
(`MIC_FILTER_HP_HZ`, `MIC_FILTER_LP_HZ`, `MIC_FILTER_DC_POLE`); and the noise
model (`MIC_NOISE_*`, `MIC_SNR_MAX_DB`).

`MIC_SPECTRAL_DECIMATION` has a consequence users see: the FFT runs on one frame
in N, so every spectral verdict — flatness, centroid, the spectral HNR — exists
at a quarter of the frame rate. That is why quality and breathiness publish the
weight they were computed on, and why two values with different weights must not
be averaged. See [Web API](API_WEB.md).

### Changing a filter cutoff invalidates the acoustic thresholds

This is not a precaution, it is an incident that already happened. The filter
chain is applied to the **stream**, before analysis, so the FFT never sees
anything but a filtered signal. A spectral statistic computed as though the full
spectrum were still there measures the filter as much as the signal: an audit
found **+14 dB of error** on the harmonic-to-noise ratio, which silently
annulled an entire threshold recalibration, and a flatness threshold that had
become *unreachable* — pure white noise, the flattest signal there is, read 0.39
against a 0.70 bound.

The `AQ_*` thresholds are therefore calibrated on synthetic PCM **passed through
the production filter chain**. Change `MIC_FILTER_HP_HZ` or `MIC_FILTER_LP_HZ`
and they must be re-measured, not carried over. The calibration tests fail on
purpose if those cutoffs move, so the re-measurement cannot be skipped by
accident.

"Calibrated on filtered synthetic PCM" is **not** "acoustically validated": no
INMP441 has ever been connected to this project. The thresholds with the
thinnest margins, and the ones most likely to move on a real microphone, are
listed in
[`AUDIO_ARCHITECTURE.md`](../Servo_flute_ESP32/docs/AUDIO_ARCHITECTURE.md)
(PHASE 6 and the audit section).

### Auto-calibration constants

`settings.h` also exposes the auto-calibration parameters
(`AUTOCAL_NOISE_RATIO`, `AUTOCAL_NOISE_MEASURE_MS`,
`AUTOCAL_PITCH_TOLERANCE_ONSET_CENTS`, `AUTOCAL_PITCH_TOLERANCE_STABLE_CENTS`,
`AUTOCAL_AIR_SETTLE_MS`, `AUTOCAL_AUDIO_FRAMES_PER_STEP`,
`AUTOCAL_REQUIRED_VALID_FRAMES`, `AUTOCAL_COARSE_STEP_PERCENT`,
`AUTOCAL_FINE_STEP_PERCENT`, `AUTOCAL_GLOBAL_TIMEOUT_MS`, …). See
`docs/AUTO_CALIBRATION.md`.

The 2026 reliability update adds:

| Constant | Role |
|----------|------|
| `AIRFLOW_SOURCE_PIVOT` | MIDI value (64) mapped to the calibrated nominal airflow during play (two-segment min→nominal→max interpolation). |
| `AUTOCAL_MIN_RESULT_CONFIDENCE` | A note whose final confidence is below this is refused and its previous calibration kept. |
| `AUTOCAL_TIMEOUT_PER_NOTE_MS` | Per-note budget; only the offending note fails, the sweep continues. |
| `AUTOCAL_GLOBAL_MARGIN_MS` | Slack added on top of `numNotes × per-note` for the computed global timeout. |
| `AUTOCAL_AUDIO_FRAME_TIMEOUT_MS` | A position aborts if no fresh microphone frame arrives in this window (frozen/disconnected source). |
| `AUTOCAL_AIR_READY_TIMEOUT_MS` | Time allowed for the air source (pump spin-up / reservoir fill / fan ramp) to become usable before the run aborts with `ACAL_FAIL_AIR_SUPPLY`. |
| `AUTOCAL_RESERVOIR_READY_MARGIN` | Reservoir (mode 5) counts as ready within this many percent of its target fill. |
| `AUTOCAL_RF_MIN_SAFE_ANGLE` / `AUTOCAL_RF_MAX_SAFE_ANGLE` / `AUTOCAL_RF_STEP_DEG` | Bounded, safe angle window the range finder sweeps (never a blind 0–180° sweep). |

The air source is abstracted behind `ICalibrationAirSupply`, so per-note calibration
runs correctly in all six air modes: the pump/reservoir/fan is brought to a
representative running state (and the noise floor measured with it running) before
any airflow position is scored. See `docs/AIR_MANAGEMENT.md` and
`docs/AUTO_CALIBRATION.md`.

## MIDI

- MIDI channel (`0` means omni);
- default CC values;
- serial MIDI enable flag and RX pin;
- MIDI file storage limit.

## WiFi

- station SSID and password;
- AP setup mode;
- mDNS hostname.

## Web UI options

- device name;
- instrument color;
- hide/show Air and Calibration tabs;
- keyboard display mode.

## Reset behavior

A normal reset restores defaults and saves them. A factory reset removes the configuration file so the first-use wizard opens again.

## 2026 runtime safety and validation update

Configuration is validated by the firmware, not only by HTML controls. PCA channel conflicts, incompatible GPIO reuse, reserved ESP32 pins, input-only output pins, invalid min/max relationships, invalid MIDI channels, invalid fingering values, and unsafe pump/sensor bounds are detected before saving.

Parameters that change `pinMode()`, I2C/PCA routing, controller `begin()` behavior, sensor setup, or serial MIDI setup require a restart. Dynamic musical values such as CC defaults, note airflow/angle percentages, fingering patterns, and temporary fan/pump test targets can be applied without hardware reinitialization.

Manual hardware tests must always be time-limited and followed by a safe state. In software-only validation, hardware procedures are documented but marked `NOT TESTED — requires hardware` in `Servo_flute_ESP32/docs/HARDWARE_TEST_MATRIX.md`.

## Real-time monophonic policy

The sequencer is intentionally monophonic. Due events are drained from the head of the MIDI queue on every update, regardless of the current note state, so an old event can never block access to later events. The priority order is:

1. panic / all-sound-off: immediate queue clear, valve close, airflow rest, fingers safe;
2. due `NOTE_ON`: replaces the current note immediately, even if `minNoteDurationMs` has not elapsed;
3. `minNoteDurationMs`: delays only a matching normal `NOTE_OFF` for the active note;
4. normal `NOTE_OFF`: stops only the active note and stale note-offs for replaced notes are ignored.

On replacement, the current valve/airflow is transitioned through the same inter-note policy as a normal stop. If the next note is within `valve_interval`, the valve may stay open while the new fingering is positioned; otherwise the valve is closed and airflow returns to rest before the new note starts.

`valve_interval` is the canonical inter-note setting. Legacy JSON using `sol_inter` is migrated on load/API update, but saves and REST responses expose only `valve_interval`.

## Finalization validation status

Software CI covers ESP32 firmware build, PlatformIO native behavior tests, pytest audits, JSON escaping regressions, MIDI 7-bit WebSocket bounds, request-size limits, diagnostics vocabulary, and supported air-management modes. Physical validation remains explicitly marked **NOT TESTED — requires hardware** until executed on the corresponding PCA9685, microphone, ToF, Hall, endstop, pump, fan, solenoid, and servo hardware.

### JSON escaping regression values

Configuration and API tests include special strings: device name `Flute "A"`, Wi-Fi SSID `atelier\\wifi`, and MIDI filename `étude "test".mid`.

## Validated value ranges

The firmware normalises or rejects every persisted value before it becomes
active. Floating-point fields are also checked with `isfinite()`, because a NaN
coming from a corrupted `/config.json` or a crafted REST body passes every
comparison and would otherwise reach the servo maths.

| Field | Range | Why |
|---|---|---|
| `vib_freq` (`vibratoFrequencyHz`) | 0.1 – 20 Hz, finite | the oscillator computes `1000 / f` then a modulo; 0, a negative value or a NaN gives a zero period (division by zero) |
| `vib_amp` (`vibratoMaxAmplitudeDeg`) | 0 – 45°, finite | a larger swing pushes the airflow servo out of its calibrated travel |
| `cc2_curve` (`cc2ResponseCurve`) | 0.1 – 4.0, finite | it is a `powf()` exponent; 0 makes the breath response constant at full scale |
| `cc_vol` / `cc_expr` / `cc_mod` / `cc_breath` / `cc_bright` / `cc2_thr` | 0 – 127 | MIDI 7-bit |
| `valve_interval`, `sol_time` | 0 – 5000 ms | absurd inter-note or coil-drive windows |
| `cc2_timeout`, `fan_idle_timeout` | 0 – 60000 ms | |
| `air_atk_ms` | 10 – 1000 ms | |
| `pump_stagger` | 0 – 5000 ms | anti-inrush delay |
| `time_unpower` | 0 – 60000 ms | |
| `sens_target` / `sens_min` / `sens_max` | 0 – 4000 mm, with `min < target < max` | a zero span divides by zero in the fill-percent computation |
| `hall_low` / `hall_high` | 0 – 4095, with `low < high` | ESP32 ADC range; equal thresholds divide by zero |
| `midi_limit` | 50 – 2000 KB | |
| `kbd_mode` | 0 – 1 | |
| `embouchure` | `trav`, `bec`, `naf`, `end`, `oca` | anything else falls back to `trav` |
| `res_format` | `balloon`, `bellows` | |
| `color` | `#RRGGBB` | |

Out-of-range numeric values are clamped and reported through `corrected: true`;
inconsistent relationships (duplicate PCA channels, duplicate MIDI notes,
`min >= max`, conflicting GPIOs) are **rejected** with HTTP 400 and leave the
running configuration untouched.

## Transactional configuration write

`POST /api/config` never mutates the running configuration while it parses: it
works on a candidate copy, validates it completely, persists it, and only then
replaces the active configuration in a single assignment. A validation failure or
a storage failure therefore changes nothing — neither the active configuration
nor the controllers. A change that needs a hardware re-init is saved but stays
inactive until the controlled reboot, so the controllers always run on a
configuration that matches the hardware actually initialised.

## Post-audit safety notes

Configuration changes are validated before application. Changes that alter GPIO assignments, PCA9685 channels, counts, air mode, reservoir sensor type, or serial MIDI RX require a restart and must be treated as pending for the next boot rather than as fully active hardware state. Legacy JSON keys are read for compatibility, but obsolete valve timing/direction fields must not be relied on for new configurations.

GPIO validation must consider active actuators and sensors; microphone builds reserve the INMP441 pins documented by the firmware. Hardware tests are still required before declaring any new wiring safe.

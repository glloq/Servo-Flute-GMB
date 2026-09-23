# Bring-up procedure — first power-on with real hardware

This document exists because the firmware cannot protect the hardware during the
window between reset and its own first instruction, and because three of the
risks found by the hardware audit are **electrical, not fixable in software**.

Read it in order. Each step is done **before** connecting what the next one
brings, and each carries a cut-off criterion: a condition under which you stop
and fix something rather than continue.

**Nothing in this firmware has ever run on real hardware.** Every figure it
relies on comes from host tests or from datasheets. That is the reason for the
staging below, not excessive caution.

---

## What the firmware guarantees, and what it cannot

**It guarantees** (verified by execution in host tests):

- `/OE` of the PCA9685 is driven HIGH — outputs disabled — as the **first
  instruction** of `setup()`, before `Serial.begin()`;
- the configured actuator GPIOs are driven to their inactive level **before**
  the I2C probe;
- servo angles are written to the PCA registers with `/OE` still disabled;
  power is enabled once, at the end, only after every channel holds a safe value;
- a missing PCA9685, an unmountable filesystem or an invalid configuration
  leaves the instrument **inert** — no path (web, MIDI, calibration) reaches an
  actuator;
- factory defaults do **not** authorise driving: a board with no `/config.json`
  refuses to move and says so on the serial port.

**It cannot guarantee** anything during the reset window — power-on, `ESP.restart()`
or a watchdog panic — because no code of this project runs then. In that window
the pin levels are whatever the pads and your external circuit decide. The
PCA9685 is **not** reset by an ESP32 reset and keeps its PWM registers.

---

## Step 0 — Electrical, before anything mechanical is connected

Nothing connected: no servo, no solenoid coil, no pump, no fan. Servo V+ rail
disconnected. PCA9685 powered on its logic supply only.

| Measure | Expected | Cut-off |
|---|---|---|
| `/OE` pull direction on the PCA board (ESP32 **unpowered**, ohmmeter to GND and to VCC) | pulled **HIGH** = outputs disabled | Common breakout boards pull `/OE` **low** = outputs **enabled**. If so, **stop**: add a pull-up that dominates, or cut the trace. Every reset would otherwise re-drive the servos to the previous note's angles for the whole bootloader. |
| Solenoid MOSFET **gate** voltage, ESP32 held in reset (EN to GND) | below the gate threshold | `SOLENOID_PIN` is GPIO13 = MTCK, which leaves reset with a **weak pull-up**, and `SOLENOID_ACTIVE_HIGH` is true — the pad wakes on the *active* side. If the gate follows, do not connect the coil until a gate pull-down is fitted. |
| Every other actuator MOSFET gate, same conditions | pulled **low** | Same reasoning. In recovery mode these pins are never driven at all. |

Scope on GPIO5 (`/OE`) and GPIO13, triggered on the rising edge of 3V3. Repeat
for all three reset causes: cold power-on, `ESP.restart()` (a "restart required"
config change from the web UI), and a watchdog reset.

---

## Step 1 — Flash, and first contact

One `pio run -t upload` over USB. There is **no filesystem image to upload**:
the web UI is compiled into the binary and there is no `data/` directory.

On a virgin ESP32 the LittleFS partition is unformatted, so the **first boot
lands in recovery mode on purpose**: `LittleFS.begin(false)` never auto-formats,
because reformatting would silently drop `/config.json` and restart the
instrument on defaults that may not match the wiring. Actuators are forbidden,
`/OE` stays HIGH.

Open the serial port at **115200**. `DeviceSecrets::printToSerial()` runs
unconditionally at every boot and prints the hotspot SSID, its WPA2 key and the
admin password. They are drawn at random on first boot and kept in NVS.

If the password is ever lost: hold BOOT at startup to regenerate and re-print
them. A headless board cannot become permanently unreachable.

From the web UI, trigger the deliberate filesystem format, then finish the setup
wizard. The controlled restart that follows is what turns the actuators on.

> A board left in **BT mode** with no configuration is inert *and* has no web
> interface. Switch to WiFi mode first. The serial message says so.

---

## Step 2 — PCA9685, still no servo

Servo V+ still disconnected. Scope on one PWM output.

- It must stay flat at 0 V for the whole boot, then produce one clean burst
  (1.5 ms / 20 ms) when power is enabled, then stop.
- **Time that burst.** It is the real exposure window during which servos are
  energised at boot, and it is *not* `time_unpower` (200 ms): `managePower()`
  only runs from `loop()`, while `setup()` still has the radio stack and — if no
  microphone is connected — up to ~600 ms of I2S timeout to go. Repeat in BT
  mode, in WiFi mode, and once with the microphone unplugged.
- Measure the **period**: it must be 20 ms. Any deviation is the PCA9685's
  internal oscillator tolerance — `setOscillatorFrequency()` is never called, so
  that deviation scales every pulse width.
- Unplug the PCA and reboot: `/OE` must stay HIGH indefinitely.

---

## Step 3 — Pulse widths, before the first horn is fitted

The servo pulse range is a **compile-time constant** (550–2450 µs for 0–180°),
not configurable and not validated against your servos.

Measure the rest pulse actually emitted on each channel and compare it with your
servo's datasheet:

| Position | Nominal pulse |
|---|---|
| valve servo closed (`valveServoCloseAngle` = 0 by default) | **550 µs** |
| airflow servo at rest (`SERVO_AIRFLOW_OFF` = 20) | **761 µs** |
| finger closed (90–100 by default) | 1500–1606 µs |

**Cut-off:** if your servo's mechanical range starts above the measured value,
do not connect it. Raise the angle in the configuration, or adjust
`SERVO_PULSE_MIN`/`MAX` in `settings.h`, until the measured pulse falls inside
the servo's range.

---

## Step 4 — One servo, current-limited, horn removed

Write your **real configuration** first. Never boot on factory defaults with
mechanics coupled — the firmware now refuses to drive on defaults, which is
exactly this protection.

Supply the servo from a current-limited bench supply set just above its rated
current, and watch the ammeter **at rest**.

**Cut-off:** a stall-level current at rest means a rest angle is against a hard
stop. Cut power and fix the configuration before fitting the horn.

Then verify `/OE` rises ~200 ms (`time_unpower`) after the last activity and the
servo goes limp. If it stays held, check that `time_unpower` is not 0 — that
value is legal, and the web UI now shows a red warning under the field
explaining that a stalled servo is then never de-energised.

Fit the horn, then the linkage, re-measuring boot current at each stage.

---

## Step 5 — Solenoid, coil last

Connect the power stage first with the coil replaced by a **dummy load** of
equivalent current. Repeat the three reset events of step 0 and watch the dummy.

The coil goes on only after that pass, and only with its **flyback diode**
fitted — the firmware cannot check for it.

- Verify the duty drops from 255 to 128 at `solenoidActivationTimeMs` (50 ms).
- With a note sounding, trigger each known `loop()` blocker in turn and measure
  how much the fallback is delayed: "reset microphone" from the UI (**at least
  100 ms, up to ~600 ms**), a 100 kB MIDI upload, a configuration save. Touch the
  coil body after each.
- Press panic **in the middle** of a microphone reset and measure the delay
  before the valve actually closes. It includes the blocking.

---

## Step 6 — Air and pump, outlet FREE

Never against a closed reservoir.

- **Reservoir mode with ToF**: unplug the sensor while regulating. The pump must
  stop within `TOF_STALE_MS` (500 ms).
- **Reservoir mode with Hall or endstop**: the pump now stops when no plausible,
  fresh, *progressing* measurement exists — but confirm it on your hardware.
  Check that `PUMP_MAX_RUN_MS` (60 s) actually covers a **complete fill** on your
  tank: it is the one value that can produce a false positive in normal use.
- Verify the endstop pull direction matches its declared active level, and that
  a long cable does not oscillate.

> **The software acts on the pump *command*, never on a pressure.** A shorted
> power stage, a latched MOSFET or a halted firmware leaves the pump running
> whatever the code decides. **A mechanical pressure-relief valve sized for the
> reservoir is the only protection independent of this firmware.**

---

## Step 7 — Degraded paths, mechanics coupled but servo supply limited

- Delete `/config.json`: the board must refuse to drive and explain why on
  serial.
- Corrupt `/config.json`: actuators locked — and **measure the pump and fan
  gates**, which on this path are never driven at all.
- Unplug the PCA: `/OE` HIGH permanently.
- Hold a note, then unplug each transport in turn (BLE, rtpMIDI, DIN) and time
  the coil current falling. DIN is only detected if the source emits Active
  Sensing; if it does not, the **30 s note ceiling** is what ends it.
- Unplug the WiFi link **during an auto-calibration** and confirm the calibrator
  stops instead of reopening the valve ~740 ms later.
- Watchdog reset during a held note, with the dummy load in place of the coil
  and the servo supply current-limited. This is the only test that reproduces
  the GPIO13 risk in real conditions.

---

## Known limitation, not fixed

`LittleFS.format()` erases 1.9 MB while the instruction cache is disabled, so
`loop()` cannot re-arm the 4 s watchdog. The documented recovery path therefore
has a good chance of being interrupted by a watchdog reset mid-format. Keep the
serial console open when using it, and expect to repeat it.

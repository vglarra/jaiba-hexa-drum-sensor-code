# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

DIVA Drum — a 6-pad ("hexa") MIDI drum trigger controller running on a Teensy 4.1. Each pad has two
sensors working together:
- **Piezo disc** — fast attack/onset detection (low latency "was it hit, how hard")
- **Velostat** — continuous pressure sensing (sustain, aftertouch, release, and a soft-touch fallback
  trigger for hits too gentle to excite the piezo)

Output: MIDI Note On/Off + per-note polyphonic aftertouch, one note per pad (scale notes C4–A#4, MIDI
channel 1), sent via Teensy's **native USB-MIDI** (`usbMIDI.sendNoteOn/sendNoteOff/sendAfterTouchPoly`).
This is USB-only output, not dual DIN+USB — the FortySevenEffects `MIDI` library and
`MIDI_CREATE_DEFAULT_INSTANCE()` have been removed entirely (see Changelog #11 for why).

Everything lives in [src/main.cpp](src/main.cpp) — this is a single-file firmware project.

## Build system

PlatformIO, not the Arduino IDE. The `pio` CLI is not on PATH in this environment — invoke it via its
full path:

```
"/c/Users/vlarr/.platformio/penv/Scripts/pio.exe" run                       # build
"/c/Users/vlarr/.platformio/penv/Scripts/pio.exe" run --target upload       # build + flash
"/c/Users/vlarr/.platformio/penv/Scripts/pio.exe" device monitor -b 115200  # serial monitor
```

Environment is `[env:teensy41]` in [platformio.ini](platformio.ini): `platform = teensy`, `board = teensy41`,
`framework = arduino`.

- `build_flags = -D USB_MIDI_SERIAL` is required — it puts the Teensy's composite USB device into a mode
  that exposes both a MIDI port and a CDC serial port simultaneously. Without it, `usbMIDI` compiles but
  no MIDI interface is exposed to the host. Don't reintroduce an external MIDI library — on Teensy, `Serial`
  is USB-CDC, not a `HardwareSerial`, so `MIDI_CREATE_DEFAULT_INSTANCE()` fails to compile against it.
- `upload_port = COM4` is pinned explicitly. PlatformIO's auto-reset-to-bootloader can pick the wrong port
  if a stale COM port from a prior session is cached, or fail with "Access is denied" if something else has
  the port open — if upload can't auto-reset, press the physical program button on the Teensy 4.1. Update
  this value if the board enumerates on a different COM port.
- PlatformIO expects an explicit `#include <Arduino.h>` in `main.cpp` — unlike the Arduino IDE, it does not
  auto-inject it.

There is no test suite ([test/](test/) only has the PlatformIO scaffold README).

## Hardware

**Piezo discs:** 35mm brass piezoceramic elements (FT-35T-2A1 type).
- Resonant frequency: 2.6 ± 0.5 kHz
- Static capacity: 35000 ± 30% pF
- Metal plate diameter 35mm, ceramic plate 25mm, total thickness 0.43mm
- Allowable input: Vp-p max 1.5–30V (rated as actuator; not relevant since used purely as a sensor here)
- Operating temp: -20°C to 70°C

**Piezo circuit (per pad):**
```
Piezo + (gold center) ──┬── [10kΩ] ──┬── Ax (analog pin)
                         │            │
                         │         [10kΩ]
                         │            │
                         │           GND
                         │
Piezo − (silver rim) ────┴──────────── GND

Protection (bipolar clamp):
  D1: 1N4148  anode → Ax    , cathode → 3.3V   (clamps positive overvoltage)
  D2: 1N4148  anode → GND   , cathode → Ax     (clamps negative rebound swing)
```
The voltage divider attenuates the piezo's raw output (can spike well above 3.3V on a hard hit) to a safe
ADC range. Both diodes are needed — piezos produce a bipolar signal (positive on compression, negative on
release), so a single diode only protects one direction.

**Velostat circuit (per pad):**
```
3.3V -- velostat -- analog pin --+-- 100Ω -- GND
```
All pads share common 3.3V and GND rails.

**Current build state:** on a breadboard. This has caused intermittent false triggers (see "Known issues"
below) — soldering to a perfboard/protoshield is the planned fix but not yet done as of this writing.

## Board notes

- Teensy 4.1, Arduino framework (see Build system above for the USB-MIDI build flag)
- `analogReadResolution(12)` **must be called explicitly** in `setup()` — Teensy 4.1 defaults to 10-bit
  `analogRead()` otherwise, which would silently invalidate every threshold in the tuning table below (all
  tuned against 12-bit counts)

## Key tuning values and why

| Constant | Value | Reasoning |
|---|---|---|
| `PIEZO_THRESHOLD` | 45 | Measured resting noise floor was ~3–24 counts (12-bit). Initially set to 30, but one pad (pad1) had a noisier floor (up to 37) due to a loose breadboard/protoboard connection — bumped to 45 for margin. Real soft hits start around 40+, so there's limited headroom; a per-pad calibrated threshold (see "Possible next steps") would let cleaner pads use a lower threshold. Still needed as of this writing — board is on a protoboard, not yet soldered, so it's still noisy; revisit once soldered and pad1's noise floor is re-measured as clean. |
| `PIEZO_MAX` | 800 | Baseline/floor for the adaptive ceiling (see below). Measured hard hits typically land 400–900; rare outliers hit 1000–2600, which now clip to velocity 127 rather than stretching the whole curve. |
| `PIEZO_PEAK_WINDOW` | 3ms | Time spent sampling after threshold crossing to find the true peak of a hit, before mapping to velocity. This is the main remaining source of intentional latency. |
| `PIEZO_DEBOUNCE` | 35ms | Minimum gap before the same pad can retrigger; caps single-pad hit rate around ~28/sec. |
| `CROSSTALK_WINDOW` | 5ms | Grouping window for the crosstalk filter (see below). |
| `CROSSTALK_RATIO` | 0.40 | A follower hit within the window must be ≥40% of the window-opening hit's peak to count as a real chord note. Measured real chords cleared 50–97%; worst observed crosstalk was 32%. |
| `ADAPTIVE_DECAY_FACTOR` | 0.985 | Per-pad adaptive ceiling decays by 1.5% every second back toward `PIEZO_MAX`. |
| `PIEZO_CURVE_EXP` | 0.45 | Piezo velocity response curve exponent (separate from the velostat's `curveExponent`). |

## Diagnostic findings (from real hardware testing)

**Crosstalk is real and mechanical.** Every hard hit bleeds into all 5 other pads within 1–4ms, at roughly
3–25% of the real hit's peak — vibration traveling through the shared mounting surface/frame, not
electrical. Real chords, by contrast, land at 50%+ of the loudest simultaneous hit. This separation is what
the crosstalk filter (0.40 ratio threshold) relies on.

**Pad1 and pad4 showed isolated false triggers** with no other pad elevated at the same time — traced to
loose/intermittent breadboard connections. Confirmed by the user: wiggling the wire reproduces random
spikes. A floating analog input picks up ambient EMI/capacitive noise; any wiggle changes contact
resistance and shows up as a spike.

## Code architecture

Two independent sensing paths feed a shared per-pad state machine, output over `usbMIDI` on channel 1.
A `Sensor` struct per pad holds both velostat state (rest/max/smoothing/touch-state machine) and piezo
state (hit detection, adaptive ceiling).

- **Piezo (fast path, `PIEZO_*` constants)**: polled every `loop()` iteration. A hit is detected once a raw
  reading crosses `PIEZO_THRESHOLD`; the code then samples for `PIEZO_PEAK_WINDOW` ms to capture the true
  peak before evaluating it (`handleNewHit` → `firePiezoNote`). Guarded so it **cannot retrigger a note
  that's already sounding** (`!s.noteOn` check) — this was a bug fix (settling vibration during a held
  chord was otherwise re-crossing the threshold and causing buzzing; see Changelog #7).
- **Velostat (`veloRest`/`veloMax`/`TouchState` per pad)**: smoothed continuously (`VEL_SMOOTHING`) and
  used for (a) a slow-attack fallback trigger for touches too soft to excite the piezo (`TOUCH_ATTACK` →
  `mapToVelocity`), and (b) once a note is on — regardless of which path triggered it — velostat owns
  **sustain** (polyphonic aftertouch, re-sent only when `mapToVelocity` output changes) and **release**
  (Note Off when pressure % drops below `RELEASE_PCT`).
- **Crosstalk filtering (`windowOpen`/`windowMaxPeak`/`CROSSTALK_WINDOW`/`CROSSTALK_RATIO`)**: the first hit
  within a `CROSSTALK_WINDOW`-ms window fires immediately (no added latency); later hits in the same window
  only fire if their peak is ≥ `CROSSTALK_RATIO` of the window-opening peak, distinguishing real chords
  from mechanical bleed-through.
- **Adaptive velocity ceiling (`piezoAdaptiveMax`, `decayAdaptiveCeilings`)**: each pad's ceiling for
  mapping peak → velocity 127 rises instantly on a new hardest-ever hit and decays slowly back toward
  `PIEZO_MAX` (every `ADAPTIVE_DECAY_INTERVAL` ms) if not re-triggered, so one outlier hit doesn't
  permanently compress that pad's dynamic range.
- **Calibration (`CalState` FSM: `CAL_IDLE` → `CAL_RESTING` → `CAL_WAITING_TOUCH` → `CAL_CAPTURING`)**:
  triggered by `calButton` (pin 3) or `'c'` over serial. Rests 3s to sample each pad's baseline, then
  auto-detects whichever pad is touched next (order-independent) and captures its peak over
  `CAPTURE_WINDOW_MS`, repeating until all pads are done. A second trigger mid-run aborts and copies a
  single representative rest/max to every pad (`abortToUniformCalibration`) for a quick uniform cal. This
  only calibrates the velostat range (`veloRest`/`veloMax`) — it has not yet been extended to per-pad piezo
  threshold calibration.
- Curve shaping (`curveExponent`, cycled via `curveButton` pin 4 through `curvePresets`) is a single shared
  exponent applied to *all* pads' velostat velocity mapping — there's no per-pad curve.

### Serial protocol (115200 baud)

Debug output and a small command interface share the same USB serial port (`handleSerial()`):
`c` (calibrate), `p` (print settings), `e <num>` (set curve exponent for all pads), `+`/`-` (nudge exponent
by 0.1). Per-pad raw/percent values print every `PRINT_INTERVAL` ms when idle.

### Gotcha: forward declarations

PlatformIO's ctags-based auto-prototype generator does not reliably insert forward declarations for every
function in this file — several functions used before their definition need explicit prototypes added
manually near the top of the file (see the block just before `setup()`). If new functions are added that
are called before their point of definition, add an explicit prototype rather than assuming it'll be
auto-generated.

## Changelog (chronological)

1. **Baseline circuit review** — confirmed piezo divider + bipolar diode clamp (D1 for overvoltage, D2
   added for negative rebound — original design only had one diode).
2. **`analogReadResolution(12)` added** — was defaulting to 10-bit.
3. **Re-tuned `PIEZO_THRESHOLD`/`PIEZO_MAX`** from real diagnostic sketch readings (rest ~3–24, soft hits
   ~40–200, hard hits ~400–2600 with rare outliers): 15→30, 500→800.
4. **Crosstalk discovered and characterized** via timestamped hit-event logging; found consistent ~3–25%
   bleed-through on every hard hit.
5. **Buffered crosstalk filter (v1)** — grouped all hits within 5ms, waited for the window to close, then
   fired only those ≥30% of the window's max. Verified against real chord data; ratio bumped to 40% after
   finding a 32% crosstalk outlier that would have slipped through at 30%.
6. **Pad1/pad4 false-trigger root cause found** — isolated noise spikes traced to loose breadboard
   connections (user confirmed by wiggling wires). `PIEZO_THRESHOLD` raised 30→45 as a partial software
   mitigation; soldering identified as the real fix (not yet done).
7. **Chord-hold buzzing bug fixed** — piezo fast-path was retriggering notes during a sustained chord hold
   due to settling vibration crossing threshold repeatedly. Fixed by gating the piezo trigger on
   `!s.noteOn`.
8. **Crosstalk filter rewritten for low latency (v2)** — original v1 added up to ~8ms of latency to *every*
   hit (3ms peak window + 5ms buffer wait), even solo hits with no crosstalk risk. Rewritten so the first
   hit in a group fires immediately; only followers within the window are delayed for the ratio check. Cut
   solo-hit latency to ~3ms (just the peak window).
9. **Per-pad adaptive velocity ceiling added** — addresses hard hits not consistently reaching velocity 127
   in the user's sampler software. Each pad's ceiling now rises instantly on a new hardest hit and decays
   slowly (1.5%/sec) back toward the tuned `PIEZO_MAX` baseline.
10. **Migrated to VS Code + PlatformIO** for git-based version control. `.ino` renamed to `src/main.cpp`;
    forward declarations added for functions used before their definition (required under strict C++
    compilation, unlike the Arduino IDE which auto-generates these).
11. **Switched from DIN MIDI to Teensy's native USB MIDI** (done in the new PlatformIO project). `MIDI`
    library and `MIDI_CREATE_DEFAULT_INSTANCE()` removed; all `MIDI.sendX()` calls replaced with
    `usbMIDI.sendX()`. Aftertouch correctly uses `sendAfterTouchPoly()` (per-note), not channel-wide
    aftertouch. Requires `build_flags = -D USB_MIDI_SERIAL` in `platformio.ini`. This is USB-only output
    now, not dual DIN+USB.
    This migration also reintroduced a regression — `PIEZO_THRESHOLD` came back as `30` instead of the
    tuned `45`, apparently from an earlier version of the sketch. Fixed back to `45` (item 12).
12. **`PIEZO_THRESHOLD` regression fixed: 30 → 45.** Confirmed still needed — board is still on a
    protoboard (not yet soldered), so pad1's noisier floor is still present.

## Known issues / open items

- **Protoboard wiring is the root cause of pad1/pad4 noise** — not yet soldered/finished. Planned fix:
  perfboard or protoshield, with strain relief (hot glue/zip-tie anchors near joints) and lightly twisted
  signal+ground pairs per pad to reduce EMI pickup. `PIEZO_THRESHOLD = 45` is the current mitigation until
  this is done.
- **No per-pad piezo threshold calibration yet.** All pads currently share one `PIEZO_THRESHOLD`. Since
  pad1's noise floor was worse than others due to the loose connection, the shared threshold is tuned
  conservatively for the worst pad, costing sensitivity on cleaner pads. Worth revisiting *after* soldering
  — if noise floors converge once wiring is fixed, a shared threshold may remain fine; if they don't, extend
  the existing velostat calibration FSM to also sample per-pad piezo noise floor.
- **USB MIDI migration done** (see Changelog #11) — DIN MIDI is no longer wired up in code at all. If a
  hardware synth ever needs to be fed directly via DIN again, that would need to be re-added alongside
  `usbMIDI`, not as a replacement for it.

## Possible next steps (not yet started)

- Solder all pad connections; re-run the raw-noise diagnostic to confirm noise floors drop and converge
  across pads.
- Per-pad piezo threshold calibration, extending the existing velostat calibration FSM.
- Re-evaluate whether `PIEZO_PEAK_WINDOW` (3ms) can be safely reduced for further latency improvement
  without degrading peak-detection accuracy.
- If a hardware synth needs direct DIN MIDI input alongside the DAW/USB connection, DIN would need to be
  re-added (`MIDI` library) running alongside `usbMIDI`, not swapped back in as a replacement.

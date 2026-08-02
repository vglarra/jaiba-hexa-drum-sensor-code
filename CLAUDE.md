# Jaiba Drum — Project Guide for Claude Code

This file gives context for an ongoing hybrid piezo + velostat MIDI drum trigger
project, built on a Teensy 4.1. It summarizes the hardware, the reasoning behind
current tuning values, and the full history of code changes so far — so future
work (in Claude Code or otherwise) starts from the same understanding.

## Project overview

6-pad MIDI drum controller. Each pad has two sensors working together:
- **Piezo disc** — fast attack/onset detection (low latency "was it hit, how hard")
- **Velostat** — continuous pressure sensing (sustain, aftertouch, release, and a
  soft-touch fallback trigger for hits too gentle to excite the piezo)

Output: MIDI Note On/Off + per-note AfterTouch (polyphonic), one note per pad
(scale notes C4–A#4, MIDI channel 1). **Now sent via Teensy's native USB MIDI**
(`usbMIDI.sendNoteOn/sendNoteOff/sendAfterTouchPoly`) — switched from DIN MIDI
for lower latency. The `MIDI` library and `MIDI_CREATE_DEFAULT_INSTANCE()` have
been removed entirely; this is USB-only, not dual DIN+USB output. Requires
`build_flags = -D USB_MIDI_SERIAL` in `platformio.ini` so the USB interface
exposes both a MIDI port and Serial (used for debug prints/calibration).

## Hardware

**Piezo discs:** 35mm brass piezoceramic elements (FT-35T-2A1 type).
- Resonant frequency: 2.6 ± 0.5 kHz
- Static capacity: 35000 ± 30% pF
- Metal plate diameter 35mm, ceramic plate 25mm, total thickness 0.43mm
- Allowable input: Vp-p max 1.5–30V (rated as actuator; not relevant since used
  purely as a sensor here)
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
The voltage divider attenuates the piezo's raw output (can spike well above
3.3V on a hard hit) to a safe ADC range. Both diodes are needed — piezos
produce a bipolar signal (positive on compression, negative on release), so a
single diode only protects one direction.

**Velostat circuit (per pad):**
```
3.3V -- velostat -- analog pin --+-- 100Ω -- GND
```
All pads share common 3.3V and GND rails.

**Current build state:** on a breadboard. This has caused intermittent false
triggers (see "Known issues" below) — soldering to a perfboard/protoshield is
the planned fix but not yet done as of this writing.

## Board / toolchain

- Teensy 4.1, Arduino framework
- `analogReadResolution(12)` — **must be called explicitly**; Teensy 4.1
  defaults to 10-bit `analogRead()` otherwise, silently invalidating any
  threshold tuned against 12-bit expectations
- MIDI library: FortySevenEffects MIDI Library (`#include <MIDI.h>`)

## Physical pad mapping

Hexagonal drum shell, 7 total mounting positions (3 currently unpopulated —
room for future pads). Physical numbering was assigned by the user during
build, not in simple reading order — recorded here from a labeled photo of
the shell.

| Pad index | Analog pin (piezo) | Analog pin (velostat) | Physical location |
|---|---|---|---|
| 0 | A6 | A0 | Second row, left |
| 1 | A7 | A1 | Second row, middle |
| 2 | A8 | A2 | Bottom row, right |
| 3 | A9 | A3 | Third row, left |
| 4 | A10 | A4 | Bottom row, left |
| 5 | A11 | A5 | Third row, right |

**Unpopulated slots (reserved for future pads):** top row (both positions),
and second row, right.

Update this table any time pads are physically rewired, repositioned, or
new ones are added to the empty slots — a stale mapping here is worse than
no mapping at all, since it looks authoritative but silently misleads.

## Key tuning values and why

| Constant | Value | Reasoning |
|---|---|---|
| `PIEZO_THRESHOLD` | 45 (⚠️ see note) | Measured resting noise floor was ~3–24 counts (12-bit). Initially set to 30, but one pad (pad1) had a noisier floor (up to 37) due to a loose breadboard connection — bumped to 45 for margin. Real soft hits start around 40+, so there's limited headroom; a per-pad calibrated threshold (see "Possible next steps") would let cleaner pads use a lower threshold. **Regression risk:** the USB MIDI migration (done by Claude Code) reintroduced `PIEZO_THRESHOLD = 30` — likely regenerated from an earlier version of the sketch. Should be restored to 45 unless pad1 has since been soldered and its noise floor re-measured as clean. |
| `PIEZO_MAX` | 800 | Baseline/floor for the adaptive ceiling (see below). Measured hard hits typically land 400–900; rare outliers hit 1000–2600, which now clip to velocity 127 rather than stretching the whole curve. |
| `PIEZO_PEAK_WINDOW` | 3ms | Time spent sampling after threshold crossing to find the true peak of a hit, before mapping to velocity. This is the main remaining source of intentional latency. |
| `PIEZO_DEBOUNCE` | 35ms | Minimum gap before the same pad can retrigger; caps single-pad hit rate around ~28/sec. |
| `CROSSTALK_WINDOW` | 5ms | Grouping window for the crosstalk filter (see below). |
| `CROSSTALK_RATIO` | 0.40 | A follower hit within the window must be ≥40% of the window-opening hit's peak to count as a real chord note. Measured real chords cleared 50–97%; worst observed crosstalk was 32%. |
| `ADAPTIVE_DECAY_FACTOR` | 0.985 | Per-pad adaptive ceiling decays by 1.5% every second back toward `PIEZO_MAX`. |
| `PIEZO_CURVE_EXP` | 0.45 | Piezo velocity response curve exponent (separate from the velostat's `curveExponent`). |

## Diagnostic findings (from real hardware testing)

**Crosstalk is real and mechanical.** Every hard hit bleeds into all 5 other
pads within 1–4ms, at roughly 3–25% of the real hit's peak — vibration
traveling through the shared mounting surface/frame, not electrical. Real
chords, by contrast, land at 50%+ of the loudest simultaneous hit. This
separation is what the crosstalk filter (0.40 ratio threshold) relies on.

**Pad1 and pad4 showed isolated false triggers** with no other pad elevated
at the same time — traced to loose/intermittent breadboard connections.
Confirmed by the user: wiggling the wire reproduces random spikes. A floating
analog input picks up ambient EMI/capacitive noise; any wiggle changes
contact resistance and shows up as a spike.

## Code architecture (current)

- `Sensor` struct per pad: holds both velostat state (rest/max/smoothing/
  touch-state machine) and piezo state (hit detection, adaptive ceiling)
- Velostat: continuous smoothed reading, trigger/release thresholds as % of
  calibrated range, owns sustain (aftertouch) and release regardless of
  whether the note was triggered by piezo or velostat
- Piezo: fast-path attack detection, guarded so it **cannot retrigger a note
  that's already sounding** (`!s.noteOn` check) — this was a bug fix, see
  changelog
- Crosstalk filter: the first hit in a timing group fires immediately
  (near-zero added latency); only subsequent hits within `CROSSTALK_WINDOW`
  are checked against the group's opening peak before being accepted or
  discarded
- Adaptive ceiling: per-pad `piezoAdaptiveMax`, rises instantly on a new
  hardest hit, decays slowly otherwise (see table above)
- Calibration FSM (velostat only, not yet extended to piezo): button/serial
  triggered, auto-detects which pad is touched, captures peak over a capture
  window, supports abort-to-uniform for a quick single-pad-based calibration
  of all pads

## Changelog (chronological)

1. **Baseline circuit review** — confirmed piezo divider + bipolar diode
   clamp (D1 for overvoltage, D2 added for negative rebound — original design
   only had one diode).
2. **`analogReadResolution(12)` added** — was defaulting to 10-bit.
3. **Re-tuned `PIEZO_THRESHOLD`/`PIEZO_MAX`** from real diagnostic sketch
   readings (rest ~3–24, soft hits ~40–200, hard hits ~400–2600 with rare
   outliers): 15→30, 500→800.
4. **Crosstalk discovered and characterized** via timestamped hit-event
   logging; found consistent ~3–25% bleed-through on every hard hit.
5. **Buffered crosstalk filter (v1)** — grouped all hits within 5ms, waited
   for the window to close, then fired only those ≥30% of the window's max.
   Verified against real chord data; ratio bumped to 40% after finding a 32%
   crosstalk outlier that would have slipped through at 30%.
6. **Pad1/pad4 false-trigger root cause found** — isolated noise spikes
   traced to loose breadboard connections (user confirmed by wiggling wires).
   `PIEZO_THRESHOLD` raised 30→45 as a partial software mitigation; soldering
   identified as the real fix (not yet done).
7. **Chord-hold buzzing bug fixed** — piezo fast-path was retriggering notes
   during a sustained chord hold due to settling vibration crossing threshold
   repeatedly. Fixed by gating the piezo trigger on `!s.noteOn`.
8. **Crosstalk filter rewritten for low latency (v2)** — original v1 added
   up to ~8ms of latency to *every* hit (3ms peak window + 5ms buffer wait),
   even solo hits with no crosstalk risk. Rewritten so the first hit in a
   group fires immediately; only followers within the window are delayed for
   the ratio check. Cut solo-hit latency to ~3ms (just the peak window).
9. **Per-pad adaptive velocity ceiling added** — addresses hard hits not
   consistently reaching velocity 127 in the user's sampler software. Each
   pad's ceiling now rises instantly on a new hardest hit and decays slowly
   (1.5%/sec) back toward the tuned `PIEZO_MAX` baseline.
10. **Migrated to VS Code + PlatformIO** for git-based version control.
    `.ino` renamed to `src/main.cpp`; forward declarations added for
    functions used before their definition (required under strict C++
    compilation, unlike the Arduino IDE which auto-generates these).
11. **Switched from DIN MIDI to Teensy's native USB MIDI** (done by Claude
    Code in the new PlatformIO project). `MIDI` library and
    `MIDI_CREATE_DEFAULT_INSTANCE()` removed; all `MIDI.sendX()` calls
    replaced with `usbMIDI.sendX()`. Aftertouch correctly uses
    `sendAfterTouchPoly()` (per-note), not channel-wide aftertouch. Requires
    `build_flags = -D USB_MIDI_SERIAL` in `platformio.ini`. This is USB-only
    output now, not dual DIN+USB.
    ⚠️ **This migration also reintroduced a regression:** `PIEZO_THRESHOLD`
    came back as `30` instead of the tuned `45` (see tuning table above) —
    appears the regenerated file was based on an earlier version of the
    sketch. Needs to be fixed before relying on this build around pad1,
    unless soldering has since resolved that pad's noise floor.
    **Update:** this was fixed — `PIEZO_THRESHOLD` is back to `45` in the
    current sketch.
12. **UART link to a second Teensy (TFT UI board) established and tested.**
    Added `Serial1.begin(115200)` (pins 0=RX1, 1=TX1) and a debug send in
    `firePiezoNote()`: `Serial1.print("PAD,"); Serial1.println(piezoPinNames[padIndex]);`.
    Confirmed end-to-end: hitting a physical pad now displays the
    corresponding analog pin label (e.g. "A6") on the separate TFT project's
    screen. This is a throwaway test protocol — see the TFT project's
    `UI_PLAN.md` for the real message protocol still to be designed (will
    need to carry note/velocity/threshold/calibration data, not just pin
    labels). Confirms both boards are correctly wired (TX↔RX crossed,
    shared GND) and that pins 0/1 don't conflict with any existing pin
    usage in this sketch.

## Known issues / open items

- **`PIEZO_THRESHOLD` regression from the USB MIDI migration** — fixed, back
  to `45`.
- **Breadboard wiring is the root cause of pad1/pad4 noise** — not yet
  soldered. Planned fix: perfboard or protoshield, with strain relief
  (hot glue/zip-tie anchors near joints) and lightly twisted signal+ground
  pairs per pad to reduce EMI pickup.
- **No per-pad piezo threshold calibration yet.** All pads currently share
  one `PIEZO_THRESHOLD`. Since pad1's noise floor was worse than others due
  to the loose connection, the shared threshold is tuned conservatively for
  the worst pad, costing sensitivity on cleaner pads. Worth revisiting *after*
  soldering — if noise floors converge once wiring is fixed, a shared
  threshold may remain fine; if they don't, extend the existing velostat
  calibration FSM to also sample per-pad piezo noise floor.
- **USB MIDI migration done** — DIN MIDI is no longer wired up in code at
  all. If a hardware synth ever needs to be fed directly via DIN again, that
  would need to be re-added alongside `usbMIDI`, not as a replacement.
- **UART link to the TFT project is active** — pins 0/1 (Serial1) are now
  in use for this and should not be repurposed for anything else in this
  sketch. Current messages sent are a throwaway test format (`PAD,<label>`);
  the real protocol (carrying note/velocity/threshold/calibration data for
  the UI) is still to be designed — see the TFT project's `UI_PLAN.md`.

## Possible next steps (not yet started)

- **Fix the `PIEZO_THRESHOLD` regression (30 → 45)** — see "Known issues."
  Quick fix, do this first.
- Solder all pad connections; re-run the raw-noise diagnostic to confirm
  noise floors drop and converge across pads.
- Per-pad piezo threshold calibration, extending the existing velostat
  calibration FSM.
- Re-evaluate whether `PIEZO_PEAK_WINDOW` (3ms) can be safely reduced for
  further latency improvement without degrading peak-detection accuracy.
- If a hardware synth needs direct DIN MIDI input alongside the DAW/USB
  connection, DIN would need to be re-added (`MIDI` library) running
  alongside `usbMIDI`, not swapped back in as a replacement.
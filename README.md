# Jaiba Drum — Hexa Drum Sensor

A 6-pad MIDI drum trigger controller built on a Teensy 4.1. Each pad combines two sensors:

- **Piezo disc** — fast attack/onset detection (low latency "was it hit, how hard")
- **Velostat** — continuous pressure sensing (sustain, aftertouch, release, and a soft-touch fallback
  trigger for hits too gentle to excite the piezo)

Output is native **USB-MIDI** (Note On/Off + per-note polyphonic aftertouch, one note per pad, channel 1)
— it shows up as a class-compliant MIDI device to any DAW, no drivers or external MIDI hardware needed.

## Hardware

**Piezo discs:** 35mm brass piezoceramic elements (FT-35T-2A1 type), used purely as sensors (not driven).

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

**Velostat circuit (per pad):**
```
3.3V -- velostat -- analog pin --+-- 100Ω -- GND
```
All pads share common 3.3V and GND rails.

Currently built on a protoboard (not yet soldered) — see `CLAUDE.md` for known noise issues this causes
and the planned fix.

## Building & flashing

This is a [PlatformIO](https://platformio.org/) project targeting `teensy41` (Teensy 4.1).

```
pio run                       # build
pio run --target upload       # build + flash
pio device monitor -b 115200  # serial monitor (debug output + commands)
```

If upload can't auto-reset the board (e.g. "Access is denied" on the COM port), close any open serial
monitor and/or press the physical **PROGRAM** button on the Teensy 4.1.

## Calibration

Trigger with the `calButton` (pin 3) or by sending `c` over serial (115200 baud):

1. First press (idle) — rests 3s to sample each pad's baseline, then waits for a touch.
2. Touch any uncalibrated pad — it's auto-detected (order doesn't matter), its peak is captured, then it
   waits for the next pad.
3. Once all pads are done, calibration finishes automatically.
4. Pressing again mid-run aborts and copies the last captured max to every pad, for a quick uniform
   calibration.

Other serial commands:
- `p` — print current settings
- `e <num>` — set the velostat curve exponent for all pads (e.g. `e 2.0`)
- `+` / `-` — nudge the curve exponent by 0.1

The `curveButton` (pin 4) cycles through a few preset curve exponents.

## Project notes

See `CLAUDE.md` for the full hardware/tuning rationale, diagnostic findings, changelog, and known issues —
useful context for anyone (or any AI assistant) picking this project back up.

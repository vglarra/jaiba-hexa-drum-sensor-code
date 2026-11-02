# Jaiba Drum — Hexa Drum Sensor

A two-Teensy MIDI drum trigger controller. Each hemisphere has its own Teensy 4.1
and appears as its own USB-MIDI device: the **right** board carries 9 dual-layer
pads, the **left** carries 7 dual-layer pads plus 5 mux'd velostat-only loop pads.
Each pad combines two sensors:

- **Piezo disc** — fast attack/onset detection (low latency "was it hit, how hard")
- **Velostat** — continuous pressure sensing (sustain, aftertouch, release, and a soft-touch fallback
  trigger for hits too gentle to excite the piezo)

Output is native **USB-MIDI** (Note On/Off + per-note polyphonic aftertouch, one note per pad, channel 1)
— it shows up as a class-compliant MIDI device to any DAW, no drivers or external MIDI hardware needed.

## Hardware

> **The control board (`PIN_MAP.md`) is the current hardware.** It uses one
> protection diode per pad (`1N4148`, anode on `PSIG`, cathode to 3V3) and a
> separate 3.3V regulator per hemisphere. The two-diode breadboard clamp drawn
> below is retired; do not wire a new board from it.

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

The two-Teensy control board described in `PIN_MAP.md` is the current hardware;
the protoboard/breadboard notes in `CLAUDE.md` are historical.

## Building & flashing

This is a [PlatformIO](https://platformio.org/) project targeting `teensy41` (Teensy 4.1).
There is one environment per hemisphere; flash one board at a time:

```
pio run -e right               # build the right hemisphere (9 pads)
pio run -e right -t upload     # build + flash it

pio run -e left                # build the left hemisphere (7 pads)
pio run -e left -t upload      # build + flash it

pio device monitor -b 115200   # serial monitor (debug output + commands)
```

The left build currently exposes the 7 dual-layer pads; the 5 mux'd loop pads
(PIN_MAP.md section 6) are not implemented yet. A build with no hemisphere flag
fails on purpose rather than falling back to the old breadboard pin order.

## Host side (sampler)

Each build presents a fixed USB-MIDI name — `Jaiba Right` / `Jaiba Left` — so
the host can tell the two ports apart without remembering a client id. MIDI
notes are disjoint:

| Board | Pads | Notes | Sampler pads |
|---|---|---|---|
| right | J_R0…J_R8 | 60, 62, … 76 | 0–8 |
| left | J_L0…J_L6 | 78, 80, … 90 | 9–15 |

Sampler pad *i* uses note `60 + 2*i`, so the two streams merge into one 16-pad
map. An empty kit with that mapping is at
`~/Documents/sounds/jaiva-kits/jaiba-16-map.jai`.

The sampler auto-detects both names on every launch, retries while a board is
absent, and reopens after a replug or power cycle. It restores the last
session/kit on startup and autosaves on quit (and every 30 s while dirty), so
pad mappings and MIDI-learned notes survive restarts.

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

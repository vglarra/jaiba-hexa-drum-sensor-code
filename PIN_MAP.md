# Jaiba control board — sensor pin map (Teensy 4.1)

> **Read this before editing `src/main.cpp`.** It is the pin map of the new
> control board and it replaces the 6-pad breadboard order the current sketch
> was written against. `sensorPins[]` and `piezoPins[]` are both replaced
> completely — the same physical pin often means something different now, so
> do not append to the old arrays.

**Source of truth:** the PCB, not this sketch. The tables below were read from
the pad/net table of `kicad-control-board/kicad-control-board.kicad_pcb` in the
`jaiba-kicad-controlboard` repo and checked with `tools/verify_board.py`, which
reports `RESULT: clean`. Board: 150 × 260 mm, three Teensy 4.1 sockets.
Snapshot date: 2026-11-02.

---

## 1. What changes

| | Current sketch | This board |
|---|---|---|
| Sensor Teensys | 1 | 2: **U_R** (right) + **U_L** (left) |
| Pads per Teensy | 6 velostat + 6 piezo | R: 9 dual-layer · L: 7 dual-layer + 5 loop |
| Analog pins used | A0–A5 (velostat), A6–A11 (piezo) | R: all 18 · L: 14 + 1 mux SIG |
| Loop pads | none | 5, velostat-only, behind a CD74HC4067 |
| Builds | one | two: `JAIBA_HEMI_R` / `JAIBA_HEMI_L` |
| Buttons | pin 3, pin 4 | unchanged: pin 3, pin 4 |
| UART to UI | Serial1, pins 0/1 | unchanged on the sensor side: Serial1, pins 0/1 |

Each sensor Teensy is its own USB-MIDI device, so the host sees two MIDI ports.
The two UART links to the UI Teensy carry monitoring/control traffic only.

**Host-side identity (implemented):** each build overrides the Teensy's USB
product string, so the ports are `Jaiba Right` and `Jaiba Left` instead of two
identical `Teensy MIDI` entries. MIDI notes are disjoint — right 60–76, left
78–90 — so a host can merge both streams into one 16-pad map. The sampler
auto-detects the two names on every start and replug; see §8a.

---

## 2. Teensy 4.1 rules you cannot break

```
A0  A1  A2  A3  A4  A5  A6  A7  A8  A9  A10 A11 A12 A13 A14 A15 A16 A17
14  15  16  17  18  19  20  21  22  23  24  25  26  27  38  39  40  41
```

| ADC name | Pin | ADC unit |
|---|---|---|
| A0 | 14 | 1 |
| A1 | 15 | 1 |
| A2 | 16 | 1 |
| A3 | 17 | 1 |
| A4 | 18 | 1 |
| A5 | 19 | 1 |
| A6 | 20 | 1 |
| A7 | 21 | 1 |
| A8 | 22 | 1 |
| A9 | 23 | 1 |
| A10 | 24 | 1 |
| A11 | 25 | 1 |
| A12 | 26 | **2** |
| A13 | 27 | **2** |
| A14 | 38 | **2** |
| A15 | 39 | **2** |
| A16 | 40 | 1 |
| A17 | 41 | 1 |

- **Pins 28–37 are not analog.** `analogRead()` returns 0 on them. Never map a
  pad there.
- Keep `analogReadResolution(12)` in `setup()` — Teensy 4.1 defaults to 10-bit
  otherwise, which silently invalidates every threshold in the sketch.
- ADC1/ADC2: `analogRead()` switches units transparently, but each switch has a
  settling cost. If a piezo peak looks noisy, group ADC1 reads and ADC2 reads
  instead of ping-ponging.
- `analogReadAveraging()` defaults to **4** samples in the Teensy 4 core. That
  is fine for velostat but rounds off piezo peaks; if velocities feel
  compressed, lower it for the piezo phase and raise it again for velostat
  (the setting is global, not per pin).
- `MAX_SENSORS = 16` is still enough for 9 (right) or 7 (left).

---

## 3. Pad connector and front end

Every drum pad is one 4-pin connector. The bare solder pads `SP_<cell>_1..4`
next to it carry the same nets, in the same order, for a cable that is soldered
rather than plugged.

```
J_R0 / J_L0 / … (4-pin)          on-board cell
1  3V3  ── velostat (in pad) ── 2  VELO ──┬── RV 100R ── GND
                                          └── W_<cell>_VELO ── wire ──> Teensy pin
3  PRAW ── R1 10k ──┬── PSIG ── W_<cell>_PSIG ── wire ──> Teensy pin
                    ├── R2 10k ── GND
                    └── D 1N4148 (anode at PSIG, cathode to 3V3)
4  GND  ── piezo −
```

- **VELO rises with pressure** (3V3 — velostat — VELO — 100R — GND). Resting
  reads near 0; a hard press approaches the 3V3 rail.
- **PSIG is a bipolar transient around 0 V.** Use peak detection, never treat it
  as a positive-only signal, and never average it into a velocity.
- Each cell has two wire landing pads, `W_<cell>_VELO` and `W_<cell>_PSIG`, and
  the Teensy pin has its own landing pad `W_R_<pin>` / `W_L_<pin>` (the wire
  goes there, not into the socket hole). That is your wiring check: cell pad →
  Teensy pad for the two pins in the table below.
- The 3V3 rail is per Teensy (`3V3_R`, `3V3_L`); each cell is fed by the
  regulator of the Teensy that reads it. Never bridge the two rails.

---

## 4. Right hemisphere — U_R (`-D JAIBA_HEMI_R`)

9 dual-layer pads, all 18 analog pins used, no spare.

| Pad | Connector | Velostat | Piezo | Placement |
|---|---|---|---|---|
| 0 | J_R0 | 23 (A9) | 16 (A2) | column A, row 1 |
| 1 | J_R1 | 22 (A8) | 15 (A1) | column A, row 2 |
| 2 | J_R2 | 21 (A7) | 14 (A0) | column A, row 3 |
| 3 | J_R3 | 20 (A6) | 41 (A17) | column A, row 4 |
| 4 | J_R4 | 19 (A5) | 40 (A16) | column A, row 5 |
| 5 | J_R5 | 18 (A4) | 39 (A15) | column A, row 6 |
| 6 | J_R6 | 17 (A3) | 38 (A14) | column A, row 7 |
| 7 | J_R7 | 24 (A10) | 26 (A12) | column A, row 8 |
| 8 | J_R8 | 25 (A11) | 27 (A13) | column B, row 1 |

```cpp
// RIGHT — index is the connector order above, not a drum order.
const int sensorPins[] = {23, 22, 21, 20, 19, 18, 17, 24, 25}; // A9  A8  A7  A6  A5  A4  A3  A10 A11
const int piezoPins[]  = {16, 15, 14, 41, 40, 39, 38, 26, 27}; // A2  A1  A0  A17 A16 A15 A14 A12 A13
```

---

## 5. Left hemisphere — U_L (`-D JAIBA_HEMI_L`)

7 dual-layer pads (column B, rows 2–8) plus the 5 loop pads in section 6.
The 7 dual-layer pads are implemented; the loop pads are not yet.

| Pad | Connector | Velostat | Piezo | Note | Placement |
|---|---|---|---|---|---|
| 0 | J_L0 | 23 (A9) | 16 (A2) | 78 | column B, row 2 |
| 1 | J_L1 | 22 (A8) | 15 (A1) | 80 | column B, row 3 |
| 2 | J_L2 | 21 (A7) | 14 (A0) | 82 | column B, row 4 |
| 3 | J_L3 | 20 (A6) | 41 (A17) | 84 | column B, row 5 |
| 4 | J_L4 | 19 (A5) | 40 (A16) | 86 | column B, row 6 |
| 5 | J_L5 | 18 (A4) | 39 (A15) | 88 | column B, row 7 |
| 6 | J_L6 | 17 (A3) | 38 (A14) | 90 | column B, row 8 |

```cpp
// LEFT
const int sensorPins[] = {23, 22, 21, 20, 19, 18, 17}; // A9  A8  A7  A6  A5  A4  A3
const int piezoPins[]  = {16, 15, 14, 41, 40, 39, 38}; // A2  A1  A0  A17 A16 A15 A14
const int scaleNotes[] = {78, 80, 82, 84, 86, 88, 90}; // disjoint from the right board
```

Left board's spare analog pins: **25, 26, 27**. Leave them free.

---

## 6. Loop pads through the mux (left build only)

> **Status: not implemented yet.** The left build currently exposes the 7
> dual-layer pads only. Pins 5–8 and 24 stay reserved for this section.

Five velostat-only loop sensors. They have **no piezo** and produce no velocity.
They share one ADC pin through a CD74HC4067 breakout.

| Loop | Connector | Mux channel | Placement |
|---|---|---|---|
| 0 | J_LOOP0 | C0 | column A, y ≈ 224 |
| 1 | J_LOOP1 | C1 | column A, y ≈ 235 |
| 2 | J_LOOP2 | C2 | column A, y ≈ 246 |
| 3 | J_LOOP3 | C3 | column B, y ≈ 224 |
| 4 | J_LOOP4 | C4 | column B, y ≈ 235 |

Connector: 2 pins, `1 = 3V3_L`, `2 = VELO`, with a 100R pulldown. Wire pad
`W_LOOPn_VELO` → mux pad `W_MUX_Cn`.

| Mux pin | Teensy 4.1 pin | Note |
|---|---|---|
| S0 | 5 | digital out |
| S1 | 6 | digital out |
| S2 | 7 | digital out |
| S3 | 8 | digital out |
| SIG | 24 (A10) | analog in |
| EN | GND | tied at the module — always enabled, no Teensy pin |
| VCC | 3V3_L | left board's rail |

Channel *i* is selected by `S3..S0 = binary i` (C0 = 0000 … C4 = 0100).
Sample one channel per 10–20 ms tick (round-robin), settle ≥ 10 µs with a
`micros()` wait, and keep the mux out of the percussive scan.

---

## 7. Physical connector order

- Two columns of connectors on the right-hand side of the board, cables arriving
  from the right edge.
- **Column A** is the inner column (closer to the Teensys), x ≈ 94 mm.
  Top → bottom: `J_R0 … J_R7`.
- **Column B** is the outer column (closer to the board edge), x ≈ 130 mm.
  Top → bottom: `J_R8`, then `J_L0 … J_L6`.
- Dual-layer rows are on a 27 mm pitch, starting at y ≈ 10 mm.
- The loop connectors are below both columns, y ≈ 224–246 mm.

The index order in the tables **is** this silkscreen order. Do not sort by pin
number or by drum pitch — the debug dump and the note table both follow the
connector.

---

## 8. UART to the UI Teensy

Both sensor boards use `Serial1` on pins 1 (TX) / 0 (RX). The UI side is where
the two links differ:

| Link | Sensor side | UI side (U_UI) |
|---|---|---|
| Right | U_R pin 1 (TX1) → | pin 0 (RX1) |
| Right | U_R pin 0 (RX1) ← | pin 1 (TX1) |
| Left | U_L pin 1 (TX1) → | pin 15 (RX3) |
| Left | U_L pin 0 (RX1) ← | pin 14 (TX3) |

**Do not move the left sensor's UART to Serial3.** The copper runs from U_L
pins 0/1, so the left build must keep `Serial1`. The UI reads the left stream
on its own `Serial3` — that is a UI-side change, not a sensor-side one.

`UART_PROTOCOL.md` says "Serial1, both boards"; that sentence is now true only
of the sensor end. Update the protocol doc before the UI work, not after.

Pad indices in `HIT`, `PADVAL`, `CAL_STATE`, `SET_NOTE`, `SET_THRESH` and
`SET_CEILING_BASELINE` grow to 0–8 (right) / 0–6 (left). The UI must know which
board a message came from (by serial port) or the two streams will collide.

---

## 8a. USB identity to the sampler (direct host path)

The sampler listens to the two sensor Teensys directly over USB, not through
the UART. Each build overrides the Teensy USB product string, so they are
distinguishable by name and need no saved port id:

| Board | USB product string | MIDI notes | Sampler pads |
|---|---|---|---|
| U_R | `Jaiba Right` | 60, 62, … 76 (9) | 0–8 |
| U_L | `Jaiba Left` | 78, 80, … 90 (7) | 9–15 |

- Sampler pad *i* is note `60 + 2*i`; the right board fills pads 0–8 and the
  left board pads 9–15. A ready-made empty kit is
  `sounds/jaiva-kits/jaiba-16-map.jai`.
- The two streams are merged; because the note ranges are disjoint, a note
  identifies its pad unambiguously.
- The sampler opens both ports by name, retries while a board is absent, and
  reopens automatically after a replug or power cycle. Only the numeric ALSA
  client id changes on replug; the name does not, which is why this survives.
- Do not change the product strings without updating the sampler's
  `autoMidiNamePrefix()`.

---

## 9. Buttons

Unchanged on both sensor boards:

| Button | Pin | Mode |
|---|---|---|
| CAL | 3 | `INPUT_PULLUP`, active low |
| CURVE | 4 | `INPUT_PULLUP`, active low |

On the left board, pins 5–8 are the mux selects and pin 24 is the mux SIG —
none of them are buttons or pads.

---

## 10. Code changes in `src/main.cpp`

1. **Pin arrays** — done: both arrays, their display names and `scaleNotes`
   are selected by build flag in `src/main.cpp`. The file `#error`s if no
   hemisphere (or both) is defined. The left branch does not define the
   loop/mux constants yet; those pins are simply unused until section 6 is
   implemented.

2. **`scaleNotes[]` must match the new count** (9 or 7). Resolved:
   right `60, 62, 64, 66, 68, 70, 72, 74, 76`; left `78, 80, 82, 84, 86, 88, 90`.
   The ranges are disjoint so the sampler can merge both MIDI streams into one
   pad map (pad *i* = note `60 + 2*i`), without any per-pad "which board" state.

3. **Compile-time guards** so a stale array cannot ship:

   ```cpp
   static_assert(sizeof(sensorPins) == sizeof(piezoPins),
                 "one piezo pin per pad");
   static_assert(sizeof(scaleNotes) / sizeof(scaleNotes[0]) ==
                 sizeof(sensorPins) / sizeof(sensorPins[0]),
                 "one note per pad");
   ```

4. **Build targets** — done: the single `[env:teensy41]` was replaced by a
   shared `[env]` plus `[env:right]` and `[env:left]`:

   ```ini
   [env]
   platform = teensy
   board = teensy41
   framework = arduino

   [env:right]
   build_flags = -D USB_MIDI_SERIAL -D JAIBA_HEMI_R

   [env:left]
   build_flags = -D USB_MIDI_SERIAL -D JAIBA_HEMI_L
   ```

   Build/flash with `pio run -e right -t upload`, then `pio run -e left -t upload`.
   Keep `-D USB_MIDI_SERIAL` in both — the sketch uses `usbMIDI`. Each build also
   overrides the USB product string (`Jaiba Right` / `Jaiba Left`, see §8a).

5. **Loop pads** — still pending. Keep them in their own small array, out of
   `sensors[]`. Every pad path in the sketch (`updateCalibration`,
   `printSettings`, the scan loop) assumes a `piezoPin` exists; a loop pad has
   none. If they must live in `sensors[]`, add a `hasPiezo` guard to *every*
   piezo read and calibration loop instead. Separate array is the smaller
   change. Pins 5–8 and 24 stay reserved until then.

6. **Calibration FSM** — loops excluded. If calibration is later persisted to
   EEPROM, tag the blob with the hemisphere so a right-board calibration cannot
   be loaded by the left build (and vice versa).

7. **Debug print cost** — `printSettings()` and the 150 ms `Serial` dump grow
   from 6 to 9 pads (right) / 7 pads (left) at 115200 baud. That print is inside
   the scan loop; if hit timing gets worse, raise `PRINT_INTERVAL` or gate the
   dump behind a debug flag rather than touching the piezo windows.

8. **Unchanged:** `analogReadResolution(12)`, `Serial1.begin(115200)`, the
   buttons, `MAX_SENSORS`, and the whole UART command set in `handleSerial1()`.

---

## 11. Note / pad assignment worksheet

Connector order is not drum order. Decide which drum zone plugs into which
connector first, then fill the notes — and check the result against the old
physical mapping in `CLAUDE.md` before renumbering anything.

**Right (U_R)**

| Pad | Connector | Drum zone | Note | Sampler pad |
|---|---|---|---|---|
| 0 | J_R0 | | 60 | 0 |
| 1 | J_R1 | | 62 | 1 |
| 2 | J_R2 | | 64 | 2 |
| 3 | J_R3 | | 66 | 3 |
| 4 | J_R4 | | 68 | 4 |
| 5 | J_R5 | | 70 | 5 |
| 6 | J_R6 | | 72 | 6 |
| 7 | J_R7 | | 74 | 7 |
| 8 | J_R8 | | 76 | 8 |

**Left (U_L)**

| Pad | Connector | Drum zone | Note | Sampler pad |
|---|---|---|---|---|
| 0 | J_L0 | | 78 | 9 |
| 1 | J_L1 | | 80 | 10 |
| 2 | J_L2 | | 82 | 11 |
| 3 | J_L3 | | 84 | 12 |
| 4 | J_L4 | | 86 | 13 |
| 5 | J_L5 | | 88 | 14 |
| 6 | J_L6 | | 90 | 15 |

Notes are fixed to make the two USB streams merge cleanly; the drum zone is
still a physical decision. An empty kit with this mapping already exists at
`sounds/jaiva-kits/jaiba-16-map.jai` (sampler pads 0–15 = notes 60–90).

**Loop (U_L, CC — number to be agreed)**

| Loop | Connector | Function | CC |
|---|---|---|---|
| 0 | J_LOOP0 | | |
| 1 | J_LOOP1 | | |
| 2 | J_LOOP2 | | |
| 3 | J_LOOP3 | | |
| 4 | J_LOOP4 | | |

---

## 12. Bench verification

1. Flash the right build. Boot banner and `p` must report **9** pads.
2. Touch each `J_R0…J_R8` in turn: only that pad's raw value moves.
3. Tap it: a `HIT` fires with the expected pad index; release sends note-off.
4. Confirm light vs hard hits give clearly different velocities.
5. Hold pressure: aftertouch keeps updating; release closes the note.
6. Buttons: CAL starts the FSM, CURVE steps the preset.
7. Left build: same 7-pad checks on `J_L0…J_L6`. The 5 loop pads are not
   implemented yet — none of the mux pins are read.
8. UART: the UI sees both boards (right on its Serial1, left on its Serial3).
9. USB: the host lists `Jaiba Right MIDI 1` and `Jaiba Left MIDI 1`; the
   sampler opens both automatically. `J_R0` sends note 60, `J_L0` note 78.
10. Leave it tapping for five minutes: no stuck notes, no phantom pads.

---

## 13. Hardware differences from the current breadboard build

- **One protection diode per pad, not two.** The board has the 1N4148 whose
  anode sits on PSIG and cathode on 3V3. The negative-clamp diode from the old
  build is gone; the Teensy's internal ESD diode catches the negative rebound,
  current-limited by R1 (10 kΩ). No firmware change, but do not expect a clean
  negative peak reading — treat anything at 0 as "no positive swing".
- **The analog node travels further.** The divider now sits at the cell and
  PSIG runs 30–130 mm to the Teensy. Expect somewhat more pickup than on the
  breadboard; per-pad thresholds and the existing crosstalk filter matter more,
  not less.
- **Separate 3.3 V rails.** Each hemisphere is powered by its own Teensy
  regulator, so the same pad can calibrate slightly differently on the two
  boards. Calibrate each board separately.

---

## 14. Do not

- Do not map any pad to pins 28–37.
- Do not reuse pins 5–8 or 24 on the left build for pads or buttons — they are
  the mux.
- Do not switch the left sensor's UART to Serial3; the copper is on pins 0/1.
- Do not put loop pads in `sensors[]` without a `hasPiezo` guard everywhere.
- Do not renumber the existing MIDI notes without asking.
- Do not add `delay()` or blocking mux settling to the percussive scan.
- Do not assume the old pad-index → drum-zone order still holds.

---

## 15. Loop pads — backlog (for later)

**Status:** hardware spec is fixed in §6; firmware and MIDI surface are not
implemented. Pins 5–8 and 24 stay reserved, and the left build currently
exposes only the 7 dual-layer pads. This is the single checklist to pick the
work up from.

### Hardware (when the loops are wired)

- 5 velostat-only cells, 2-pin connectors: `1 = 3V3_L`, `2 = VELO`, 100R
  pulldown. No piezo, so no velocity and no piezo aftertouch.
- `W_LOOPn_VELO` → mux `W_MUX_Cn` (C0–C4). EN tied to GND at the module;
  VCC from `3V3_L`; S0–S3 = Teensy pins 5–8; SIG = pin 24 (A10).

### Firmware

- [ ] `LoopSensor` array kept **separate from `sensors[]`** — a loop has no
      `piezoPin`, and every pad path assumes one.
- [ ] Mux select helper: `S3..S0 = binary channel`; settle ≥ 10 µs with
      `micros()` before `analogRead(SIG)`.
- [ ] Round-robin one channel per 10–20 ms tick, outside the percussive scan;
      no `delay()` anywhere in the scan.
- [ ] Smoothing + per-loop rest/trigger/release values. No velocity; decide
      between on/off and continuous pressure output.
- [ ] Keep loops out of the existing calibration FSM (§10.6), or add a
      separate loop calibration step and say so here.
- [ ] Decide the MIDI surface **before** writing the send code: CC (numbers
      TBD) or notes 91–95 for the sampler's 5 global pads. Do not invent
      numbers in firmware until the worksheet in §11 is filled.
- [ ] Define loop UART messages if the TFT needs them. The current
      `HIT`/`PADVAL`/`CAL_STATE` set only covers kit pads.
- [ ] If EEPROM calibration persistence is added later, tag loop calibration
      with the hemisphere as well.
- [ ] Keep `NUM_SENSORS`/`MAX_SENSORS` for the dual-layer kit pads and give
      loops their own `NUM_LOOPS = 5`; do not grow `sensors[]`.

### Sampler side

- The app already has exactly 5 global loop pads
  (`PadManager::kNumGlobalPads = 5`), disabled by default (`midiNote=-1`).
- If loops send notes 91–95, map one note per global pad 1:1 — no sampler
  code change beyond the mapping.
- If loops send CC, the sampler's global pads are note-triggered today, so a
  CC handler (or a CC→note bridge) is a small new feature.
- Global pads live in `.gjm` sessions, not in a standalone `.jai` kit.

### Bench when implemented

- [ ] Press one loop at a time: only the selected mux channel moves, and all
      five refresh within ~100 ms.
- [ ] The 7 dual-layer pads show no phantom hits while the mux is scanning.
- [ ] Five minutes of loop use: no stuck loops, no channel bleed.
- [ ] Check the UART stream (if used) and the sampler's global pads
      independently from the kit pads.

### Open decisions

1. CC numbers vs notes 91–95 (and whether a loop outputs pressure/CC
   continuously or just on/off).
2. Loop calibration inside the existing FSM or as a separate pass.
3. UART message shape for loop state, if the TFT will show it.
4. Whether loop calibration needs EEPROM persistence alongside the pads.

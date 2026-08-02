# UART Protocol — Drum Teensy ↔ TFT Teensy

Shared reference for both projects. Both sides must implement exactly this
— if either side needs to deviate, update this file first so it stays the
single source of truth.

## Transport

- `Serial1`, 115200 baud, both boards
- Pins 0 (RX1) / 1 (TX1) on both, wired crossed (TX↔RX), shared GND
- One message per line, newline (`\n`) terminated. `println()` on Arduino/
  Teensy sends `\r\n` — receivers should `trim()` incoming lines to drop
  the trailing `\r` (already proven working in the test protocol).
- Max line length: keep messages well under 64 chars. Receivers should
  discard/reset on any line exceeding a sane limit (the test code already
  does this at 63 chars — keep that guard).

## General message shape

```
<COMMAND>,<arg1>,<arg2>,...\n
```

All-caps command name, comma-separated arguments, no spaces. Pad indices
are always the 0-based index (0–5 currently, matching `sensors[]`), not
the analog pin label — the pin-label format from the test protocol is
retired.

---

## Drum Teensy → TFT (events and query responses)

### `HIT,<padIndex>,<velocity>`
Sent every time a note fires (from either piezo or velostat path), for the
future Hit Monitor screen and any live feedback elsewhere in the UI.
Example: `HIT,4,76`

### `PADVAL,<padIndex>,<note>,<threshold>,<ceiling>,<curveExp>`
Response to a `GET_PAD` query (see below). Reports current live state for
one pad. `ceiling` is the current `piezoAdaptiveMax` (a float, decays over
time, so this is a live-updating read, not a fixed config value).
Example: `PADVAL,4,68,45,823,0.45`

### `CAL_STATE,<state>`
Sent whenever the calibration FSM changes state, so the Calibration screen
can mirror it instead of polling or guessing. `<state>` is one of:
`RESTING`, `WAITING_TOUCH`, `CAPTURING,<padIndex>`, `PAD_DONE,<padIndex>`,
`COMPLETE`.
Examples: `CAL_STATE,RESTING` / `CAL_STATE,CAPTURING,2` /
`CAL_STATE,PAD_DONE,2` / `CAL_STATE,COMPLETE`

### `XTALKVAL,<ratio>,<windowMs>`
Response to a `GET_XTALK` query. Reports current `CROSSTALK_RATIO` and
`CROSSTALK_WINDOW`.
Example: `XTALKVAL,0.40,5`

### `ACK,<command>`
Sent after successfully applying any `SET_*` command from the TFT, so the
UI can confirm a change actually took effect rather than assuming success.
Example: `ACK,SET_THRESH`

### `ERR,<reason>`
Sent if a received command couldn't be applied (bad pad index, malformed
message, etc). `<reason>` is a short human-readable string, not a code.
Example: `ERR,BAD_PAD_INDEX`

---

## TFT → Drum Teensy (commands)

### `SET_NOTE,<padIndex>,<note>`
Assign a pad's MIDI note. Example: `SET_NOTE,3,65`

### `SET_THRESH,<padIndex>,<threshold>`
Set `PIEZO_THRESHOLD` for one pad. Note: the current drum sketch has
`PIEZO_THRESHOLD` as a single shared value across all pads, not per-pad —
this command implies per-pad thresholds, which isn't implemented yet on
the drum side. Flag this as a prerequisite change before this command can
actually do anything per-pad (see "Open items" below).
Example: `SET_THRESH,1,50`

### `SET_CEILING_BASELINE,<padIndex>,<value>`
Set the floor/baseline a pad's adaptive ceiling decays back toward
(currently the global `PIEZO_MAX`). Same per-pad caveat as above.
Example: `SET_CEILING_BASELINE,1,800`

### `SET_CURVE,<exponent>`
Set the piezo curve exponent directly (a float). Applies globally, matching
current sketch behavior (`PIEZO_CURVE_EXP` is shared, not per-pad).
Example: `SET_CURVE,0.50`

### `SET_XTALK,<ratio>,<windowMs>`
Set `CROSSTALK_RATIO` and `CROSSTALK_WINDOW` together.
Example: `SET_XTALK,0.45,5`

### `CAL_START`
Equivalent to the existing serial `c` command / physical calibration
button press. Starts the velostat calibration sequence.

### `GET_PAD,<padIndex>`
Request current values for one pad. Drum Teensy responds with `PADVAL,...`
(see above).

### `GET_XTALK`
Request current crosstalk settings. Drum Teensy responds with
`XTALKVAL,...` (see above).

---

## Open items — drum sketch changes needed before some commands work

The protocol above assumes some things the drum sketch doesn't do yet:

1. **Per-pad `PIEZO_THRESHOLD` and ceiling baseline.** Currently both are
   single shared values (`int PIEZO_THRESHOLD`, `int PIEZO_MAX`) used by
   all pads. `SET_THRESH` and `SET_CEILING_BASELINE` in this protocol
   assume per-pad storage (e.g. moving these into the `Sensor` struct).
   This is a real code change to the drum sketch, not just adding a serial
   command handler — worth doing as its own step, and it also ties into
   the "per-pad piezo calibration" item already listed as a possible next
   step in the drum project's `CLAUDE.md`.
2. **A serial command parser needs to exist at all on the drum side.**
   `handleSerial()` currently only parses the USB Serial commands (`c`,
   `e <num>`, `+`, `-`, `p`). A new parser needs to read `Serial1` and
   dispatch on the command names above — this is new code, not a small
   tweak.
3. **`CAL_STATE` messages need to be added at each FSM transition point.**
   The calibration FSM (`startCalibration()`, `finishRestPhase()`,
   `updateCalibration()`'s capture-complete block) doesn't currently emit
   anything — these sends need to be added at each relevant transition.

## Suggested implementation order

1. Drum Teensy: add per-pad threshold/ceiling-baseline storage (structural
   change, do this before wiring up the corresponding commands)
2. Drum Teensy: add the `Serial1` command parser, implement `SET_NOTE`,
   `SET_CURVE`, `SET_XTALK`, `CAL_START` first (these don't depend on the
   per-pad storage change)
3. Drum Teensy: implement `SET_THRESH`, `SET_CEILING_BASELINE`,
   `GET_PAD`/`PADVAL` once per-pad storage exists
4. Drum Teensy: add `CAL_STATE` sends at FSM transitions
5. Drum Teensy: add `HIT` sends (can reuse/replace the existing test
   `Serial1.print("PAD,"...)` line in `firePiezoNote()`)
6. TFT: replace the test protocol's `PAD,<label>` parsing with real
   parsing for `HIT`, `PADVAL`, `CAL_STATE`, `XTALKVAL`, `ACK`, `ERR`
7. TFT: wire each screen's stubbed `sendCommand()` calls (from
   `UI_PLAN.md`'s suggested build order) to actually send the real
   commands above
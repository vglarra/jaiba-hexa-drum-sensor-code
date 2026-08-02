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

### `HIT,<padIndex>,<velocity>,<source>`
Sent every time a note fires (from either piezo or velostat path), for the
future Hit Monitor screen and any live feedback elsewhere in the UI.
`<source>` is `P` (piezo fast-path) or `V` (velostat soft-touch fallback) —
useful for diagnosing tuning issues, e.g. a pad frequently triggering via
`V` on normal (not soft) hits suggests its piezo threshold is set too high.
Examples: `HIT,4,76,P` / `HIT,2,31,V`

### `PADVAL,<padIndex>,<note>,<threshold>,<ceiling>,<curveExp>`
Response to a `GET_PAD` query (see below). Reports current live state for
one pad. `ceiling` is the current `piezoAdaptiveMax` (a float, decays over
time, so this is a live-updating read, not a fixed config value).
Example: `PADVAL,4,68,45,823,0.45`

### `CAL_STATE,<sensor>,<state>`
Sent whenever the calibration FSM changes state, so the Calibration screen
can mirror it instead of polling or guessing. `<sensor>` is `PIEZO` or
`VELOSTAT` (calibration always runs for exactly one sensor type at a time,
never both). `<state>` is one of: `RESTING`, `WAITING_TOUCH`,
`CAPTURING,<padIndex>`, `PAD_DONE,<padIndex>`, `COMPLETE`, `ABORTED`.
Examples: `CAL_STATE,VELOSTAT,RESTING` / `CAL_STATE,PIEZO,CAPTURING,2` /
`CAL_STATE,PIEZO,PAD_DONE,2` / `CAL_STATE,VELOSTAT,COMPLETE` /
`CAL_STATE,PIEZO,ABORTED`

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

### `CAL_START,<sensor>,<scope>[,<padIndex>]`
Starts a calibration run. Replaces the old argument-less `CAL_START`.

- `<sensor>`: `PIEZO` or `VELOSTAT` — which sensor type to calibrate.
  Never both at once; starting one while the other is mid-run should
  `ERR` rather than silently interrupting it.
- `<scope>`:
  - `ALL` — calibrate every pad in sequence, auto-detecting which pad was
    touched (this is the existing velostat behavior, generalized to also
    apply to piezo). `<padIndex>` omitted.
  - `SINGLE` — calibrate exactly one specified pad, leave all others
    untouched. Requires `<padIndex>`.
  - `SINGLE_COPY_ALL` — calibrate one specified pad, then apply that same
    result to every other pad immediately on completion. Requires
    `<padIndex>`. This is the same effective result as the old
    `abortToUniformCalibration()` behavior, but as a deliberate first-class
    action instead of an incidental side effect of pressing the button
    again mid-run.
- `<padIndex>`: 0-based pad index, required for `SINGLE`/`SINGLE_COPY_ALL`,
  omitted for `ALL`.

Examples: `CAL_START,VELOSTAT,ALL` / `CAL_START,PIEZO,SINGLE,2` /
`CAL_START,PIEZO,SINGLE_COPY_ALL,0`

Sending `CAL_START` again while a run is already in progress aborts it
(same as the existing button/serial `c` behavior) — sends
`CAL_STATE,<sensor>,ABORTED` for whichever sensor's run was active.

### `GET_PAD,<padIndex>`
Request current values for one pad. Drum Teensy responds with `PADVAL,...`
(see above).

### `GET_XTALK`
Request current crosstalk settings. Drum Teensy responds with
`XTALKVAL,...` (see above).

---

## Piezo calibration — new concept, mirrors velostat's shape

Velostat calibration already has two phases: **rest** (measure ambient
noise floor per pad) and **capture** (measure peak on a hard hit). Piezo
calibration should mirror this exact shape, applied to the fields already
added for piezo (`piezoThreshold`, `piezoCeilingBaseline`) instead of
velostat's (`veloRest`, `veloMax`):

- **Rest phase**: sample each pad's piezo line with nothing touching it,
  same as velostat's `CAL_RESTING`. Result becomes the new
  `piezoThreshold` (likely noise floor + a margin, mirroring how
  `DETECT_MARGIN` works for velostat's touch detection — exact margin is
  an implementation decision, not specified here).
- **Capture phase**: same as velostat's `CAL_CAPTURING` — hit the pad as
  hard as you'll ever play, capture the peak. Result becomes the new
  `piezoCeilingBaseline`, and (per the existing `SET_CEILING_BASELINE`
  behavior) should immediately snap `piezoAdaptiveMax` to match, not wait
  for decay.

This is real new drum-side logic, not a protocol-only change — the FSM
states, rest-phase sampling, and capture-phase peak-tracking all need
piezo-specific implementations alongside (not replacing) the existing
velostat ones, since a user may want to calibrate either independently.

### `SET_PLAY_MODE,<mode>`
**Not yet implemented — queued as the next feature after the current
`DETECT_MARGIN` fix.** Lets the player restrict which sensor path can
trigger notes, for isolating one sensor type while testing/feeling it out.
`<mode>` is one of `BOTH` (default, current behavior — either sensor can
trigger), `PIEZO_ONLY`, `VELOSTAT_ONLY`. Global, not per-pad.

Implementation sketch (not finalized): gate the piezo fast-path's trigger
condition and the velostat's `TOUCH_ATTACK` trigger condition each behind
a check against the current mode — e.g. piezo's `if (!s.hitting && !s.noteOn
&& ...)` gains `&& playMode != VELOSTAT_ONLY`, and velostat's equivalent
`TOUCH_IDLE` trigger gains `&& playMode != PIEZO_ONLY`. Sustain/release/
aftertouch logic (which is velostat-owned regardless of trigger origin)
likely stays unaffected — worth confirming during implementation whether
`VELOSTAT_ONLY`/`PIEZO_ONLY` should also suppress aftertouch reporting,
or just note-triggering.

Fits naturally under a future **Settings** screen (currently a disabled
placeholder footer icon in `UI_PLAN.md`) rather than Tuning, since it's a
global play-behavior toggle, not a per-parameter calibration value.

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
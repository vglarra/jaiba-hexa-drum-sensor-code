#include <Arduino.h>

// Uses Teensy's native USB-MIDI (usbMIDI), not an external MIDI library —
// requires build_flags = -D USB_MIDI_SERIAL in platformio.ini so the USB
// interface exposes both a MIDI port and Serial (used below for debug prints).

// ============================================================
// VELOSTAT SCALE — AUTO-DETECT CALIBRATION, 6 SENSORS
// + PIEZO CROSSTALK FILTERING (v2)
//
// CHANGELOG from previous version:
//   - analogReadResolution(12) added (was defaulting to 10-bit)
//   - PIEZO_THRESHOLD: 15 -> 30   (measured noise floor was ~3-24 counts)
//   - PIEZO_MAX:       500 -> 800 (measured hard-hit range was ~400-900,
//                                   extreme outliers 1000-2600 now clip to 127)
//   - NEW: hit buffering + relative-amplitude crosstalk filter.
//     Every hard hit was found to bleed into all other pads within 1-4ms,
//     at roughly 3-25% of the real hit's peak (measured on the shared
//     mounting surface). Real chords, by contrast, land at 50%+ of the
//     loudest simultaneous hit. So: buffer all hits within a short window,
//     then only fire the ones that clear CROSSTALK_RATIO of that window's
//     loudest hit. This preserves chords while rejecting crosstalk.
//
// Calibration flow (button on calButton, or 'c' over Serial):
//   1st press (idle)      -> start: rest 3s, then wait for touch
//   touch an uncalibrated pad -> auto-detects it, captures peak,
//                                 marks it done, waits for next
//   touch an already-done pad -> tells you, keeps waiting
//   all pads done          -> calibration finishes automatically
//   press again (idle)     -> starts over from scratch
//   press again (mid-run)  -> ABORTS and copies the last captured
//                              max to every pad (quick uniform cal)
//
// Wiring per pad (3.3V logic):
//   3.3V -- velostat -- analog pin --+-- 100ohm -- GND
// All pads share the same 3.3V and GND rails.
//
// Serial Monitor commands (115200 baud):
//   c        -> same as pressing the button
//   e <num>  -> set curve exponent for ALL pads, e.g. "e 2.0"
//   +  / -   -> nudge exponent by 0.1
//   p        -> print current settings
// ============================================================

const int sensorPins[] = { A0, A1, A2, A3, A4, A5 };
const int piezoPins[]  = { A6, A7, A8, A9, A10, A11 }; // one piezo per pad
const int scaleNotes[] = { 60, 62, 64, 66, 68, 70 }; // C major-ish, C4-A#4

const int NUM_SENSORS = sizeof(sensorPins) / sizeof(sensorPins[0]);
const int MAX_SENSORS = 16; // static allocation ceiling, raise if needed

const int calButton   = 3;
const int curveButton = 4;   // press to cycle through curve presets

// --- Smoothing (shared) ---
const float VEL_SMOOTHING = 0.15f;

// --- Curve shaping (shared across all pads) ---
// Press curveButton to step through these in order, wrapping around.
const float curvePresets[] = { 1.0f, 1.4f, 1.8f, 2.2f, 2.6f, 3.0f };
const int NUM_CURVE_PRESETS = sizeof(curvePresets) / sizeof(curvePresets[0]);
int curvePresetIndex = 2; // starts at 1.8, matching the old default
float curveExponent = curvePresets[curvePresetIndex];

// --- Trigger / release thresholds, as % of range above rest ---
const float TRIGGER_PCT = 0.05f;
const float RELEASE_PCT = 0.02f;

// --- Attack capture window (for normal note playing) ---
const unsigned long ATTACK_WINDOW_MS = 30;
const unsigned long MIDI_UPDATE_INTERVAL = 15;

// --- Calibration detect / capture tuning (velostat) ---
const int   DETECT_MARGIN       = 20;   // raw counts above rest = "you touched this pad"
const unsigned long CAPTURE_WINDOW_MS = 1200; // how long to sample peak once detected

// --- Piezo hit detection (fast path, low latency) ---
// Re-tuned from real 12-bit readings (rest ~3-24, soft hits ~40-200,
// hard hits ~400-900, rare outliers up to ~2600 which now clip to 127).
int PIEZO_THRESHOLD = 45;     // raw ADC counts above baseline = "that's a hit"
int PIEZO_MAX       = 800;    // raw peak that maps to velocity 127
const unsigned long PIEZO_DEBOUNCE   = 35;  // ms — min gap between hits on the same pad
const unsigned long PIEZO_PEAK_WINDOW = 3;  // ms — how long to sample the true peak of a hit
const float PIEZO_CURVE_EXP = 0.45f;        // piezo response curve (separate from velostat's)

// --- Adaptive velocity ceiling (per pad) ---
// PIEZO_MAX above is the starting/floor ceiling. Any hit harder than a
// pad's current ceiling instantly becomes the new ceiling for that pad, so
// your hardest hit always maps to velocity 127 — no more guessing one fixed
// number that's sometimes too low (undershoots 127) or too high (compresses
// your normal-intensity range). The ceiling decays slowly back toward
// PIEZO_MAX if you stop hitting that hard, so a single outlier hit doesn't
// permanently flatten your dynamics for the rest of the session.
const unsigned long ADAPTIVE_DECAY_INTERVAL = 1000;  // ms between decay steps
const float ADAPTIVE_DECAY_FACTOR = 0.985f;          // ceiling *= this, each interval
unsigned long lastAdaptiveDecayTime = 0;

// --- Crosstalk filtering (low-latency version) ---
// The FIRST hit to arrive opens a short window and fires immediately —
// measured data shows the real strike always arrives before any crosstalk
// it causes, so there's no need to delay it. Any further hits that land
// within CROSSTALK_WINDOW ms of that first hit are only accepted (fired)
// if they're >= CROSSTALK_RATIO of the window-opening hit's peak; weaker
// ones are discarded as mechanical bleed-through. This keeps chords intact
// while cutting solo-hit latency down to just the peak-finding window.
const unsigned long CROSSTALK_WINDOW = 5;   // ms
const float CROSSTALK_RATIO = 0.40f;        // measured chords cleared 50-97%; worst crosstalk was 32%

bool windowOpen = false;
unsigned long windowStart = 0;
int windowMaxPeak = 0;

enum TouchState { TOUCH_IDLE, TOUCH_ATTACK, TOUCH_HELD };

struct Sensor {
  int pin;
  int note;
  float smoothed;
  int veloRest;
  int veloMax;
  TouchState touchState;
  unsigned long attackStart;
  float attackPeak;
  int lastSentVelocity;
  unsigned long lastMidiSend;
  bool noteOn;          // true once a note is sounding, from either piezo or velostat

  // --- Piezo (fast-path attack detection) ---
  int piezoPin;
  bool hitting;
  int piezoPeak;
  unsigned long peakTime;
  unsigned long lastHitTime;
  float piezoAdaptiveMax; // rises instantly on a harder-than-ever hit, decays slowly otherwise
};

Sensor sensors[MAX_SENSORS];

// --- Calibration FSM ---
enum CalState { CAL_IDLE, CAL_RESTING, CAL_WAITING_TOUCH, CAL_CAPTURING };
CalState calState = CAL_IDLE;

bool calibratedFlag[MAX_SENSORS];
bool wasAboveDetect[MAX_SENSORS];   // for "already calibrated" one-shot notice
int  activePad = -1;
unsigned long captureStart = 0;
float capturePeak = 0;
int  lastCompletedMax = -1;         // most recent finished pad's max (for abort-uniform)

unsigned long calRestStart = 0;
long calRestTotal[MAX_SENSORS];
int  calRestSamples[MAX_SENSORS];
const unsigned long CAL_REST_DURATION = 3000;

unsigned long lastPrint = 0;
const unsigned long PRINT_INTERVAL = 150;

void printSettings();
void onCalibrationTrigger();
void startCalibration();
void abortToUniformCalibration();

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 2000);
  analogReadResolution(12);
  pinMode(calButton, INPUT_PULLUP);
  pinMode(curveButton, INPUT_PULLUP);

  for (int i = 0; i < NUM_SENSORS; i++) {
    sensors[i].pin = sensorPins[i];
    sensors[i].note = scaleNotes[i];
    sensors[i].smoothed = 0;
    sensors[i].veloRest = 20;
    sensors[i].veloMax = 900;
    sensors[i].touchState = TOUCH_IDLE;
    sensors[i].attackStart = 0;
    sensors[i].attackPeak = 0;
    sensors[i].lastSentVelocity = -1;
    sensors[i].lastMidiSend = 0;
    sensors[i].noteOn = false;

    sensors[i].piezoPin = piezoPins[i];
    sensors[i].hitting = false;
    sensors[i].piezoPeak = 0;
    sensors[i].peakTime = 0;
    sensors[i].lastHitTime = 0;
    sensors[i].piezoAdaptiveMax = (float)PIEZO_MAX;
  }

  Serial.print("=== Velostat scale ("); Serial.print(NUM_SENSORS); Serial.println(" sensors) ===");
  Serial.println("Commands: c=calibrate  e <num>=set exponent  +/-=nudge  p=print");
  Serial.println("Buttons: calButton=calibrate  curveButton=cycle curve preset");
  Serial.println("------------------------------------");
  printSettings();
}

void printSettings() {
  Serial.print("exponent="); Serial.println(curveExponent, 2);
  for (int i = 0; i < NUM_SENSORS; i++) {
    Serial.print("  pad "); Serial.print(i);
    Serial.print(" (note "); Serial.print(sensors[i].note); Serial.print(")");
    Serial.print("  rest="); Serial.print(sensors[i].veloRest);
    Serial.print("  max="); Serial.print(sensors[i].veloMax);
    Serial.print("  piezoCeil="); Serial.println(sensors[i].piezoAdaptiveMax, 0);
  }
}

void handleSerial() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;

  if (line == "c") {
    onCalibrationTrigger();
  } else if (line == "p") {
    printSettings();
  } else if (line == "+") {
    curveExponent += 0.1f;
    printSettings();
  } else if (line == "-") {
    curveExponent = max(0.1f, curveExponent - 0.1f);
    printSettings();
  } else if (line.startsWith("e ")) {
    float v = line.substring(2).toFloat();
    if (v > 0) { curveExponent = v; printSettings(); }
  }
}

// Edge-triggered, debounced button reads — fire once per physical press.
// Two separate functions (not a shared struct type) so Teensy/Arduino's
// auto-generated function prototypes — inserted near the top of the file,
// before any of our own struct definitions — don't choke on an unknown type.
bool calLastReading = HIGH, calDebouncedState = HIGH;
unsigned long calLastChangeTime = 0;

bool calButtonPressedEdge() {
  bool reading = digitalRead(calButton);
  if (reading != calLastReading) {
    calLastChangeTime = millis();
    calLastReading = reading;
  }
  if (millis() - calLastChangeTime > 40 && reading != calDebouncedState) {
    calDebouncedState = reading;
    if (calDebouncedState == LOW) return true;
  }
  return false;
}

bool curveLastReading = HIGH, curveDebouncedState = HIGH;
unsigned long curveLastChangeTime = 0;

bool curveButtonPressedEdge() {
  bool reading = digitalRead(curveButton);
  if (reading != curveLastReading) {
    curveLastChangeTime = millis();
    curveLastReading = reading;
  }
  if (millis() - curveLastChangeTime > 40 && reading != curveDebouncedState) {
    curveDebouncedState = reading;
    if (curveDebouncedState == LOW) return true;
  }
  return false;
}

void onCurveButtonTrigger() {
  curvePresetIndex = (curvePresetIndex + 1) % NUM_CURVE_PRESETS;
  curveExponent = curvePresets[curvePresetIndex];
  Serial.print("Curve exponent -> "); Serial.println(curveExponent, 2);
}

// ---- Single entry point for "the button/command was triggered" ----
void onCalibrationTrigger() {
  if (calState == CAL_IDLE) {
    startCalibration();
  } else {
    abortToUniformCalibration();
  }
}

void startCalibration() {
  Serial.println("=== CALIBRATION STARTED ===");
  Serial.println(">>> Don't touch any pad. Resting for 3 sec...");
  for (int i = 0; i < NUM_SENSORS; i++) {
    calRestTotal[i] = 0;
    calRestSamples[i] = 0;
    calibratedFlag[i] = false;
    wasAboveDetect[i] = false;
  }
  lastCompletedMax = -1;
  activePad = -1;
  calRestStart = millis();
  calState = CAL_RESTING;
}

void finishRestPhase() {
  for (int i = 0; i < NUM_SENSORS; i++) {
    sensors[i].veloRest = (calRestSamples[i] > 0) ? (int)(calRestTotal[i] / calRestSamples[i]) : 20;
    Serial.print("Pad "); Serial.print(i); Serial.print(" rest = "); Serial.println(sensors[i].veloRest);
  }
  Serial.println(">>> Touch ANY pad and press it as hard as you'll ever play.");
  Serial.println("    (order doesn't matter — I'll figure out which pad it is)");
  calState = CAL_WAITING_TOUCH;
}

// Copies one representative calibration to every pad and ends the run early
void abortToUniformCalibration() {
  int uniformMax;
  if (calState == CAL_CAPTURING && activePad >= 0) {
    // Use whatever peak has been seen so far on the pad currently being captured
    uniformMax = (int)capturePeak;
  } else if (lastCompletedMax > 0) {
    uniformMax = lastCompletedMax;
  } else {
    uniformMax = 900; // no data yet at all — fall back to a safe default
  }

  // Average rest across pads that finished the rest phase (falls back to defaults if aborted super early)
  long restSum = 0;
  for (int i = 0; i < NUM_SENSORS; i++) restSum += sensors[i].veloRest;
  int uniformRest = restSum / NUM_SENSORS;

  for (int i = 0; i < NUM_SENSORS; i++) {
    sensors[i].veloRest = uniformRest;
    sensors[i].veloMax = uniformMax;
  }

  Serial.println("=== CALIBRATION ABORTED — applying uniform values to ALL pads ===");
  Serial.print("uniform rest="); Serial.print(uniformRest);
  Serial.print("  uniform max="); Serial.println(uniformMax);
  printSettings();

  calState = CAL_IDLE;
  activePad = -1;
}

void updateCalibration() {
  unsigned long now = millis();

  if (calState == CAL_RESTING) {
    for (int i = 0; i < NUM_SENSORS; i++) {
      calRestTotal[i] += analogRead(sensors[i].pin);
      calRestSamples[i]++;
    }
    if (now - calRestStart >= CAL_REST_DURATION) {
      finishRestPhase();
    }
    return;
  }

  if (calState == CAL_WAITING_TOUCH) {
    for (int i = 0; i < NUM_SENSORS; i++) {
      int raw = analogRead(sensors[i].pin);
      bool above = raw > sensors[i].veloRest + DETECT_MARGIN;

      if (above && !wasAboveDetect[i]) {
        // Fresh touch onset on pad i
        wasAboveDetect[i] = true;
        if (calibratedFlag[i]) {
          Serial.print("Pad "); Serial.print(i); Serial.println(" already calibrated — try a different pad.");
        } else {
          activePad = i;
          capturePeak = raw;
          captureStart = now;
          calState = CAL_CAPTURING;
          Serial.print(">>> Detected pad "); Serial.print(i); Serial.println(" — capturing peak, keep pressing...");
          break; // handle one pad at a time
        }
      } else if (!above) {
        wasAboveDetect[i] = false;
      }
    }
    return;
  }

  if (calState == CAL_CAPTURING) {
    int raw = analogRead(sensors[activePad].pin);
    if (raw > capturePeak) capturePeak = raw;

    if (now - captureStart >= CAPTURE_WINDOW_MS) {
      int rest = sensors[activePad].veloRest;
      sensors[activePad].veloMax = (capturePeak > rest + 20) ? (int)capturePeak : rest + 200;
      calibratedFlag[activePad] = true;
      lastCompletedMax = sensors[activePad].veloMax;

      Serial.print("Pad "); Serial.print(activePad);
      Serial.print(" calibrated. max="); Serial.println(sensors[activePad].veloMax);

      int doneCount = 0;
      for (int i = 0; i < NUM_SENSORS; i++) if (calibratedFlag[i]) doneCount++;
      Serial.print(doneCount); Serial.print(" of "); Serial.print(NUM_SENSORS); Serial.println(" pads done.");

      activePad = -1;
      for (int i = 0; i < NUM_SENSORS; i++) wasAboveDetect[i] = false; // require full release before next detect

      if (doneCount >= NUM_SENSORS) {
        Serial.println("=== CALIBRATION COMPLETE ===");
        printSettings();
        calState = CAL_IDLE;
      } else {
        Serial.println(">>> Touch the next uncalibrated pad.");
        calState = CAL_WAITING_TOUCH;
      }
    }
    return;
  }
}

int mapToVelocity(Sensor &s, float pressure) {
  float range = (float)(s.veloMax - s.veloRest);
  if (range < 1) range = 1;
  float norm = (pressure - s.veloRest) / range;
  norm = constrain(norm, 0.0f, 1.0f);
  float mapped = pow(norm, curveExponent);
  return constrain((int)(mapped * 127), 0, 127);
}

int getPiezoVelocity(int peak, float ceiling) {
  if (peak <= PIEZO_THRESHOLD) return 0;
  float clamped = constrain((float)peak, (float)PIEZO_THRESHOLD, ceiling);
  float norm = (clamped - PIEZO_THRESHOLD) / (ceiling - PIEZO_THRESHOLD);
  float mapped = pow(norm, PIEZO_CURVE_EXP);
  return constrain((int)(mapped * 127), 1, 127);
}

// Actually fires a piezo-triggered note.
void firePiezoNote(int padIndex, int peak) {
  Sensor &s = sensors[padIndex];

  // A new hardest hit instantly raises this pad's ceiling, so THIS hit
  // itself maps to exactly 127 rather than clipping against an old ceiling.
  if ((float)peak > s.piezoAdaptiveMax) {
    s.piezoAdaptiveMax = (float)peak;
  }

  int hitVelocity = getPiezoVelocity(peak, s.piezoAdaptiveMax);
  if (hitVelocity >= 3) {
    usbMIDI.sendNoteOn(s.note, hitVelocity, 1);
    s.lastSentVelocity = hitVelocity;
    s.noteOn = true;
    Serial.print("PAD "); Serial.print(padIndex);
    Serial.print(" PIEZO HIT  note="); Serial.print(s.note);
    Serial.print("  peak="); Serial.print(peak);
    Serial.print("  ceil="); Serial.print(s.piezoAdaptiveMax, 0);
    Serial.print("  vel="); Serial.println(hitVelocity);
  }
}

// Called the instant a hit's peak has been captured. If no window is
// currently open (or the previous one has expired), this hit starts a new
// window and fires right away — no added latency for solo hits. If a
// window is already open (i.e. this hit landed within CROSSTALK_WINDOW ms
// of the hit that opened it), it only fires if it's a strong enough
// fraction of that hit's peak to be a real simultaneous strike (chord)
// rather than mechanical crosstalk.
void handleNewHit(int padIndex, int peak) {
  unsigned long now = millis();

  if (!windowOpen || (now - windowStart > CROSSTALK_WINDOW)) {
    windowOpen = true;
    windowStart = now;
    windowMaxPeak = peak;
    firePiezoNote(padIndex, peak); // first hit in the window — fire immediately
    return;
  }

  // Inside an already-open window: only accept if strong enough relative
  // to the hit that opened it.
  if (peak >= CROSSTALK_RATIO * windowMaxPeak) {
    firePiezoNote(padIndex, peak); // real chord note
  }
  // else: rejected as crosstalk, discarded silently
}

// Called once per loop; every ADAPTIVE_DECAY_INTERVAL ms, each pad's ceiling
// relaxes slightly toward the tuned baseline (PIEZO_MAX). This means a
// single outlier-hard hit doesn't permanently compress your normal dynamic
// range — if you don't keep hitting that hard, sensitivity gradually comes
// back. The ceiling never decays below PIEZO_MAX itself.
void decayAdaptiveCeilings() {
  unsigned long now = millis();
  if (now - lastAdaptiveDecayTime < ADAPTIVE_DECAY_INTERVAL) return;
  lastAdaptiveDecayTime = now;

  for (int i = 0; i < NUM_SENSORS; i++) {
    float decayed = sensors[i].piezoAdaptiveMax * ADAPTIVE_DECAY_FACTOR;
    sensors[i].piezoAdaptiveMax = max((float)PIEZO_MAX, decayed);
  }
}

void loop() {
  handleSerial();

  if (calButtonPressedEdge()) {
    onCalibrationTrigger();
  }
  if (curveButtonPressedEdge()) {
    onCurveButtonTrigger();
  }

  if (calState != CAL_IDLE) {
    updateCalibration();
    return;
  }

  unsigned long now = millis();
  decayAdaptiveCeilings();
  bool doPrint = (now - lastPrint >= PRINT_INTERVAL);
  if (doPrint) lastPrint = now;

  for (int i = 0; i < NUM_SENSORS; i++) {
    Sensor &s = sensors[i];

    // ---------- PIEZO: fast attack detection (low latency) ----------
    // Guarded with !s.noteOn so a note that's already sustaining (e.g. held
    // during a chord) can't be retriggered by small settling vibration —
    // only a fresh hit after release should start a new piezo attack.
    int rawPiezo = analogRead(s.piezoPin);
    if (!s.hitting && !s.noteOn && rawPiezo > PIEZO_THRESHOLD && (now - s.lastHitTime > PIEZO_DEBOUNCE)) {
      s.hitting = true;
      s.piezoPeak = rawPiezo;
      s.peakTime = now;
      // A real hit is starting — cancel any in-progress velostat attack capture
      // so it doesn't also fire a duplicate, slower note right after this one.
      s.touchState = TOUCH_IDLE;
    } else if (s.hitting) {
      if (rawPiezo > s.piezoPeak) s.piezoPeak = rawPiezo;
      if (now - s.peakTime >= PIEZO_PEAK_WINDOW) {
        s.hitting = false;
        s.lastHitTime = now;
        // Evaluate immediately — fires right away if it's the first hit in
        // a window, or checked against that hit's peak if it's a follower.
        handleNewHit(i, s.piezoPeak);
      }
    }

    // ---------- VELOSTAT: sustain / release, and soft-touch fallback trigger ----------
    int raw = analogRead(s.pin);
    s.smoothed = s.smoothed * (1.0f - VEL_SMOOTHING) + raw * VEL_SMOOTHING;

    float range = (float)(s.veloMax - s.veloRest);
    if (range < 1) range = 1;
    float pct = (s.smoothed - s.veloRest) / range;

    if (!s.noteOn) {
      // No note sounding yet — velostat can still trigger one for touches too
      // soft to excite the piezo (e.g. slow presses with no sharp attack).
      if (s.touchState == TOUCH_IDLE) {
        if (pct >= TRIGGER_PCT) {
          s.touchState = TOUCH_ATTACK;
          s.attackStart = now;
          s.attackPeak = s.smoothed;
        }
      } else if (s.touchState == TOUCH_ATTACK) {
        if (s.smoothed > s.attackPeak) s.attackPeak = s.smoothed;
        if (now - s.attackStart >= ATTACK_WINDOW_MS) {
          int hitVelocity = mapToVelocity(s, s.attackPeak);
          usbMIDI.sendNoteOn(s.note, max(hitVelocity, 1), 1);
          s.lastSentVelocity = hitVelocity;
          s.noteOn = true;
          s.touchState = TOUCH_HELD;
          Serial.print("PAD "); Serial.print(i);
          Serial.print(" VELO TRIGGER  note="); Serial.print(s.note);
          Serial.print("  vel="); Serial.println(hitVelocity);
        }
      }
    } else {
      // Note is sounding (from piezo or velostat) — velostat now owns
      // sustain (continuous aftertouch) and release, regardless of origin.
      if (pct <= RELEASE_PCT) {
        usbMIDI.sendNoteOff(s.note, 0, 1);
        s.noteOn = false;
        s.touchState = TOUCH_IDLE;
        Serial.print("PAD "); Serial.print(i); Serial.println(" NOTE OFF");
      } else if (now - s.lastMidiSend >= MIDI_UPDATE_INTERVAL) {
        int heldVelocity = mapToVelocity(s, s.smoothed);
        if (heldVelocity != s.lastSentVelocity) {
          usbMIDI.sendAfterTouchPoly(s.note, heldVelocity, 1); // per-note aftertouch
          s.lastSentVelocity = heldVelocity;
        }
        s.lastMidiSend = now;
        s.touchState = TOUCH_HELD;
      }
    }

    if (doPrint) {
      Serial.print("pad"); Serial.print(i);
      Serial.print(" raw="); Serial.print(raw);
      Serial.print(" pct="); Serial.print(pct * 100, 0);
      Serial.print("%  ");
    }
  }

  if (doPrint) Serial.println();
}
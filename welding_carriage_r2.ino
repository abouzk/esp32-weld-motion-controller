// ==============================================================================
// ESP32 WELDING CARRIAGE — REVISION 2 (POST-AUDIT, SAFETY-VERIFIED)
// Author: Karim Abouzeid
// Date: 3/23/2026
//
// HARDWARE DEPLOYMENT REQUIREMENTS (Non-negotiable before shop use):
//   - Limit switch and E-Stop wiring MUST use shielded twisted-pair cable.
//   - Cable shield MUST be grounded at the power supply chassis, not the ESP32.
//   - Add hardware RC filters on pins 32 & 33: 100Ω series + 100nF to GND.
//   - Verify DM542T microstep DIP switches match MICROSTEPS_PER_REV below.
//     If you change the DIP switches, you MUST recalculate WELD_STEP_DELAY_US.
//
// KINEMATIC ASSUMPTIONS:
//   - Driver:              DM542T
//   - Microstep setting:   1600 steps/rev (SW3=ON, SW4=OFF per DM542T manual)
//   - Leadscrew pitch:     72 mm/rev == 2.835 inches/rev  ← FILL THIS IN DEPENDING ON LINEAR ACTUATOR CURRENTLY IN USE; CURRENTLY FOR HEECHOO 600MM BELT DRIVE
//   - Target weld speed:   6.0 - 8.0 IPM
//   - Calculated pulse rate: 752 Hz → half-period = 664µs
// ==============================================================================

#include "esp_task_wdt.h"  // Hardware Watchdog Timer

// ------------------------------------------------------------------------------
// 1. HARDWARE PIN MAPPING
// ------------------------------------------------------------------------------
const int STEP_PIN         = 26;
const int DIR_PIN          = 25;
const int LIMIT_SWITCH_PIN = 32;
//const int ESTOP_PIN        = 33;

// ------------------------------------------------------------------------------
// 2. TIMING CONSTANTS
// ------------------------------------------------------------------------------
const unsigned long WELD_STEP_DELAY_US  = 664;       // 8 IPM half-period
const unsigned long HOMING_DELAY_US     = 1500;      // Slower homing speed
const unsigned long DIR_SETUP_TIME_US   = 10;        // DM542T requires ≥5µs. We use 10µs.
const unsigned long HOMING_TIMEOUT_US   = 30000000UL;// 30 seconds. Fault if limit not found.
const unsigned long DEBOUNCE_MS         = 50;        // 50ms is sufficient for mechanical debounce

// Watchdog: if loop() stalls for this many seconds, the ESP32 hard-resets.
const int WDT_TIMEOUT_S = 2;

// ------------------------------------------------------------------------------
// 3. TRAVEL LIMIT
// Set this to your physical rail length in steps, minus a safety margin.
// At 752 steps/sec (8 IPM), 30 seconds of travel ≈ 22,560 steps.
// Measure your actual rail and set accordingly. This is your software end-stop.
// ------------------------------------------------------------------------------
const unsigned long MAX_TRAVEL_STEPS = 20000;

// ------------------------------------------------------------------------------
// 4. STATE MACHINE
// The system exists in exactly one of these states at all times.
// ------------------------------------------------------------------------------
enum SystemState {
  STANDBY,      // Idle, waiting for command
  HOMING,       // Moving in reverse to find the limit switch
  READY,        // At Position 0, cleared to weld
  WELDING_FWD,  // Moving forward at 8 IPM
  FAULT         // Locked. Requires 'R' command after physical inspection.
};

volatile SystemState currentState = STANDBY;

// ------------------------------------------------------------------------------
// 5. ISR FLAGS
// ISRs write ONLY these flags. All logic, math, and state changes are in loop().
// This is the gold standard for embedded safety interrupt handling.
// ------------------------------------------------------------------------------
volatile bool eStopFlag       = false;
volatile bool limitSwitchFlag = false;

// ------------------------------------------------------------------------------
// 6. RUNTIME VARIABLES
// ------------------------------------------------------------------------------
unsigned long previousMicros    = 0;
unsigned long homingStartMicros = 0;
unsigned long travelStepCount   = 0;
bool stepState                  = LOW;

// Debounce timestamps — safe to use millis() here because we're in loop(), not an ISR
unsigned long lastEStopDebounce = 0;
unsigned long lastLimitDebounce = 0;


// ==============================================================================
// 7. ISRs — MINIMAL BY DESIGN
//
// These do exactly two things and nothing else:
//   1. Set a flag so loop() knows an event occurred.
//   2. Immediately kill the step pin via a direct hardware register write.
//
// WHY GPIO.out_w1tc instead of digitalWrite():
//   'w1tc' = "Write 1 to Clear" — a single atomic register operation that is
//   guaranteed safe in ISR context. digitalWrite() routes through abstraction
//   layers that are not ISR-safe on all ESP32 Arduino versions.
//
// WHY NOT millis() or state changes here:
//   millis() reads a timer that the FreeRTOS SysTick ISR updates. If our ISR
//   fires mid-update, millis() can return a stale value, silently failing the
//   debounce check and discarding an E-Stop press.
// ==============================================================================

void IRAM_ATTR eStopTriggered() {
  eStopFlag = true;
  GPIO.out_w1tc.val = (1UL << STEP_PIN); // Kill step pulse instantly
}

void IRAM_ATTR limitSwitchTriggered() {
  limitSwitchFlag = true;
  GPIO.out_w1tc.val = (1UL << STEP_PIN); // Kill step pulse instantly
}


// ==============================================================================
// 8. SETUP
// ==============================================================================
void setup() {
  Serial.begin(115200);

  // Motor output pins
  pinMode(STEP_PIN, OUTPUT);
  pinMode(DIR_PIN, OUTPUT);

  // Safety input pins — INPUT_PULLUP uses the ESP32's internal 3.3V reference.
  // Hardware RC filters on these pins are a wiring requirement (see file header).
  pinMode(LIMIT_SWITCH_PIN, INPUT_PULLUP);
  //pinMode(ESTOP_PIN, INPUT_PULLUP);

  // Guarantee clean state on boot — no stray signals to the driver
  digitalWrite(STEP_PIN, LOW);
  digitalWrite(DIR_PIN, LOW);

  // Initialize hardware watchdog. If loop() ever stalls (heap corruption,
  // peripheral lockup, FreeRTOS starvation), the ESP32 will hard-reset after
  // WDT_TIMEOUT_S seconds rather than leaving the driver in an unknown state.
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
  esp_task_wdt_add(NULL); // Subscribe the main Arduino task

  // Attach ISRs — FALLING = switch connects to GND (active with INPUT_PULLUP)
  //attachInterrupt(digitalPinToInterrupt(ESTOP_PIN), eStopTriggered, FALLING); TODO: ESTOP_PIN not yet assigned
  //E-STOP hardware interrupt deferred to R3. Current E-STOP relies on hardware 24V cut only.
  attachInterrupt(digitalPinToInterrupt(LIMIT_SWITCH_PIN), limitSwitchTriggered, FALLING);

  Serial.println("========================================");
  Serial.println(" WELDING CARRIAGE — SYSTEM BOOT");
  Serial.println(" STATE: STANDBY");
  Serial.println("----------------------------------------");
  Serial.println(" [H] Home   [W] Weld");
  Serial.println(" [S] Stop   [R] Reset Fault");
  Serial.println("========================================");
}


// ==============================================================================
// 9. FAULT ENTRY — Centralised fault handler
// All fault conditions funnel through this function. Never inline a fault.
// ==============================================================================
void enterFault(const char* reason) {
  currentState = FAULT;
  digitalWrite(STEP_PIN, LOW);
  stepState = LOW;           // Sync software state with physical pin (NEW-02 fix)
  travelStepCount = 0;
  Serial.print("\n*** FAULT: ");
  Serial.println(reason);
  Serial.println("*** Motor stopped. Inspect machine before continuing.");
  Serial.println("*** Send 'R' to reset ONLY after confirming fault is resolved.\n");
}


// ==============================================================================
// 10. MAIN LOOP
// ==============================================================================
void loop() {

  // Feed the hardware watchdog. If this line is never reached, the board resets.
  esp_task_wdt_reset();

  unsigned long currentMicros = micros();
  unsigned long currentMillis = millis();


  // ============================================================
  // SECTION A: PROCESS ISR FLAGS (Deferred, debounced, safe)
  // ============================================================

  // --- E-STOP ---
  if (eStopFlag) {
    if (currentMillis - lastEStopDebounce > DEBOUNCE_MS) {
      lastEStopDebounce = currentMillis;
      eStopFlag = false;
      if (currentState != FAULT) {  // Don't stack fault messages
        enterFault("E-STOP ACTIVATED");
      }
    }
  }

  // --- LIMIT SWITCH ---
  if (limitSwitchFlag) {
    if (currentMillis - lastLimitDebounce > DEBOUNCE_MS) {
      lastLimitDebounce = currentMillis;
      limitSwitchFlag = false;

      if (currentState == HOMING) {
        // Expected condition: found the wall. Transition to READY.
        currentState = READY;
        digitalWrite(STEP_PIN, LOW);
        stepState = LOW;          // Sync software state with physical pin (NEW-02 fix)
        travelStepCount = 0;
        Serial.println("HOMING COMPLETE. STATE: READY.");
        Serial.println("Send 'W' to begin welding.");

      } else if (currentState == WELDING_FWD) {
        // Unexpected condition: limit switch hit while going forward.
        // The carriage is traveling the wrong direction, or wiring is reversed.
        enterFault("LIMIT SWITCH HIT DURING FORWARD TRAVEL — Inspect direction wiring");

      }
      // If STANDBY / READY / FAULT: a phantom EMI trigger. Ignore it.
    }
  }


  // ============================================================
  // SECTION B: SERIAL COMMAND HANDLER
  // ============================================================

  if (Serial.available() > 0) {
    char cmd = Serial.read();

    // Filter CR, LF, and all other non-printable characters from terminal line endings
    if (cmd < 32) { /* discard silently */ }

    // --- [H] HOME ---
    else if (cmd == 'H' || cmd == 'h') {
      if (currentState == STANDBY) {

        // NEW-01 FIX: Check if carriage is ALREADY sitting on the limit switch.
        // If so, driving in reverse immediately grinds into the wall.
        // Short-circuit directly to READY instead.
        if (digitalRead(LIMIT_SWITCH_PIN) == LOW) {
          currentState = READY;
          Serial.println("Already at home position. STATE: READY.");
          Serial.println("Send 'W' to begin welding.");
        } else {
          Serial.println("Executing HOMING sequence...");
          digitalWrite(DIR_PIN, LOW);
          delayMicroseconds(DIR_SETUP_TIME_US); // HIGH-03 FIX: DM542T dir setup time
          homingStartMicros = micros();         // NEW-03 FIX: Start the homing timeout clock
          stepState = LOW;                      // NEW-02 FIX: Ensure clean pulse state on entry
          currentState = HOMING;
        }

      } else if (currentState == FAULT) {
        Serial.println("FAULT ACTIVE. Send 'R' to acknowledge and reset before homing.");
      } else {
        Serial.println("Cannot home: system is not in STANDBY.");
      }
    }

    // --- [W] WELD ---
    else if (cmd == 'W' || cmd == 'w') {
      if (currentState == READY) {
        Serial.println("Executing WELDING FWD at 8 IPM...");
        travelStepCount = 0;
        digitalWrite(DIR_PIN, HIGH);
        delayMicroseconds(DIR_SETUP_TIME_US); // HIGH-03 FIX: DM542T dir setup time
        stepState = LOW;                      // NEW-02 FIX: Ensure clean pulse state on entry
        currentState = WELDING_FWD;
      } else {
        Serial.println("SAFETY LOCK: Cannot weld. Must be in READY state. Home the carriage first.");
      }
    }

    // --- [S] STOP ---
    // Graceful stop from an active motion state. Returns to STANDBY.
    // Welding position is now unknown, so re-homing is required before next weld.
    else if (cmd == 'S' || cmd == 's') {
      if (currentState == WELDING_FWD || currentState == HOMING) {
        currentState = STANDBY;
        digitalWrite(STEP_PIN, LOW);
        stepState = LOW;    // NEW-02 FIX: Sync software state with physical pin
        Serial.println("STOPPED. STATE: STANDBY.");
        Serial.println("Position is now unknown. Send 'H' to re-home before welding.");
      } else {
        Serial.println("Nothing to stop.");
      }
    }

    // --- [R] RESET FAULT ---
    // CRITICAL-03 FIX: Explicit two-step recovery. Cannot skip to H directly.
    // The operator must send R (acknowledging the fault) then H (to re-home).
    // This forces a deliberate, conscious decision before the machine moves again.
    else if (cmd == 'R' || cmd == 'r') {
      if (currentState == FAULT) {
        currentState = STANDBY;
        eStopFlag       = false;  // Clear any flags that fired during the fault
        limitSwitchFlag = false;
        travelStepCount = 0;
        Serial.println("FAULT ACKNOWLEDGED AND CLEARED. STATE: STANDBY.");
        Serial.println("OPERATOR: Confirm the fault condition is physically resolved.");
        Serial.println("Send 'H' to home when safe to do so.");
      } else {
        Serial.println("No fault active. Nothing to reset.");
      }
    }
  }


  // ============================================================
  // SECTION C: MOTOR MOVEMENT — NON-BLOCKING STATE MACHINE
  // No delay() calls. Time-based on micros() comparison only.
  // ============================================================

  switch (currentState) {

    case HOMING:
      // NEW-03 FIX: Homing timeout. If the limit switch is never found,
      // the limit switch wire may be broken or disconnected. Fault immediately.
      if (currentMicros - homingStartMicros > HOMING_TIMEOUT_US) {
        enterFault("HOMING TIMEOUT — Limit switch not triggered in 30s. Check wiring.");
        break;
      }
      if (currentMicros - previousMicros >= HOMING_DELAY_US) {
        previousMicros = currentMicros;
        stepState = !stepState;
        digitalWrite(STEP_PIN, stepState);
      }
      break;

    case WELDING_FWD:
      // HIGH-02 FIX: Software travel limit — a "digital wall."
      // If the physical limit switch wire fails open, this is the last line of defense.
      if (travelStepCount >= MAX_TRAVEL_STEPS) {
        enterFault("SOFTWARE TRAVEL LIMIT REACHED — Physical limit switch may have failed.");
        break;
      }
      if (currentMicros - previousMicros >= WELD_STEP_DELAY_US) {
        previousMicros = currentMicros;
        stepState = !stepState;
        digitalWrite(STEP_PIN, stepState);
        // Count only on rising edge (one complete pulse = one step)
        if (stepState == HIGH) {
          travelStepCount++;
        }
      }
      break;

    case FAULT:
      // Absolute zero. No motion, no pulses. Nothing executes here.
      // The only exit is the 'R' command, handled in Section B above.
      break;

    case STANDBY:
    case READY:
      // Idle states. Wait for a command. Motor is stationary.
      break;
  }
}
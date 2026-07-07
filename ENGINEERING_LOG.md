# Automated Weld Motion Controller -- Engineering Log

**Production Code:** [esp32-weld-motion-controller](https://github.com/abouzk/esp32-weld-motion-controller)

---

### 2026-03-27: Firmware Revision 2 -- Post-Audit Safety Implementation

* **Objective:** Implement motion control firmware incorporating all findings from the March 18 hardware audit, with emphasis on ISR safety architecture and hardware fault detection.
* **Architecture Decisions:**
  + ISR functions declared with `IRAM_ATTR`
    - *Reasoning:* Forces ISR residence in SRAM rather than flash. Flash read operations can be suspended during cache misses, causing ISR execution delays incompatible with safety-critical interrupt handling. All logic, state changes, and timing are deferred to `loop()` -- ISRs set boolean flags only.
  + `GPIO.out_w1tc.val = (1UL << STEP_PIN)` used inside ISRs instead of `digitalWrite()`
    - *Reasoning:* `w1tc` (Write-1-to-Clear) is a single atomic register operation guaranteed safe in ISR context. `digitalWrite()` routes through abstraction layers that are not ISR-safe on all ESP32 Arduino versions. Ensures step pulse is killed instantly on E-STOP or limit switch trigger.
  + Hardware watchdog timer initialized at 2-second timeout
    - *Reasoning:* If `loop()` ever stalls due to heap corruption, peripheral lockup, or FreeRTOS task starvation, the ESP32 hard-resets rather than leaving the DM542T driver in an unknown commanded state with no recovery path.
  + 30-second homing timeout (`HOMING_TIMEOUT_US`) implemented as a fault trigger
    - *Reasoning:* Direct response to the March 18 bracket finding. If the limit switch is not triggered within 30 seconds of homing initiation, the system faults with an explicit wire/bracket diagnostic message rather than running indefinitely.
  + Software travel limit (`MAX_TRAVEL_STEPS`) implemented as a digital end-stop
    - *Reasoning:* If the physical limit switch fails open during forward travel, the step counter is the last line of defense before the carriage runs off the rail.
  + Two-step fault recovery: `R` command (acknowledge) required before `H` command (re-home)
    - *Reasoning:* Prevents operator from bypassing acknowledgment and immediately restarting motion after a safety event. Forces a deliberate decision before the machine moves again.
* **Blockers / Constraints:**
  + `ESTOP_PIN` not yet assigned in firmware -- `attachInterrupt` for software E-STOP is currently non-functional (compile error). Hardware E-STOP 24V cut remains active as primary stop. Software E-STOP ISR deferred to Revision 3.
  + GPIO pin assignment (code: 25/26/27) must be verified against physical wiring before first hardware test.
  + Belt drive pitch circumference placeholder in firmware header not yet filled in.
  + `HOLD` and `RETRACT` states not yet implemented -- current firmware goes `WELDING_FWD` to `FAULT` or manual stop. Planned for Revision 3.
  + First motion test pending oscilloscope validation of 565 Hz pulse frequency.
* **Next Action Items:**
  + Fix `ESTOP_PIN` compile issue (assign pin or defer with explicit TODO comment).
  + Verify GPIO pins against physical board wiring.
  + Fill pitch circumference value in firmware header (72mm = 2.835 in/rev).
  + Oscilloscope validation of STEP pulse frequency against 565 Hz target.
  + First actuator motion test -- no weld, motion only.

---

### 2026-03-24: State Machine & Architecture Diagrams -- Digitization

* **Objective:** Digitize hand-drawn system architecture and state machine into mermaid diagrams for repository documentation.
* **Architecture Decisions:**
  + E-STOP modeled as a `note` on the `WELDING` state in the documentation diagram rather than explicit transition arrows from every state
    - *Reasoning:* E-STOP is a hardware interrupt that fires from any state -- it is not a named transition between defined states. Explicit arrows from every state to a FAULT node would clutter the diagram without adding information. The firmware implementation uses an explicit FAULT state internally; the documentation diagram prioritizes clarity over implementation detail.
  + Five-subgraph block diagram structure: Human Interaction Layer, Low-Voltage Logic, Power Distribution, High-Current Drive, Electromechanical Layer
    - *Reasoning:* Subgraph grouping visually enforces the three electrical domain isolation constraint (3.3V logic / 24V motion / 220V welding). Makes the EMI isolation architecture immediately readable without prose explanation.
* **Blockers / Constraints:**
  + None -- diagrams are documentation artifacts only, no hardware dependency.
* **Next Action Items:**
  + Finalize firmware Revision 2 incorporating March 18 audit findings.
  + Validate pulse frequency on oscilloscope against 565 Hz target.

---

### 2026-03-18: Speed Analysis, Microstepping Configuration & Hardware Safety Audit

* **Objective:** Translate the 6-8 IPM system requirement into stepper motor pulse parameters, select DM542T microstepping configuration, and conduct a physical hardware safety audit of the existing linear actuator assembly.
* **Architecture Decisions:**
  + 1/8 microstepping (1600 PPR) selected over full-step (200 steps/rev)
    - *Reasoning:* At full-step resolution, minimum pulse frequency = (21.2 RPM x 200) / 60 = 70.7 Hz. This falls within the NEMA 23 primary mechanical resonance band -- observed effects include step loss, audible vibration, and unpredictable instantaneous speed, directly violating the <5% velocity variance requirement. At 1/8 microstep, minimum frequency = (21.2 x 1600) / 60 = 565.3 Hz, well above the resonance band. Upper bound: 752 Hz at 8 IPM -- within DM542T reliable pulse range.
  + Pin assignment corrected: GPIO 25 (STEP/PUL-), GPIO 26 (DIR-), GPIO 27 (ENA-), GPIO 32 (Limit Switch -- TBD, two switches available). Initial R2 had STEP/DIR swapped -- caught this before first hardware test.
    - *Reasoning:* Pins selected to avoid GPIO 34-39 (input-only on ESP32 DevKit) and GPIO 0/2/15 (boot-strapping pins that affect flash programming if pulled low at boot).
* **Blockers / Constraints:**
  + Hardware Safety Finding (ISO 14971 Hazard Analysis) -- Stress fracturing identified at the inner corner junctions of the U-bracket where the bracket legs meet the top mounting bar. This geometry concentrates bending stress at exactly the points where the bracket is most loaded during carriage impact at home position. Two small screws retain the bracket to the actuator end plate -- if the plastic cracks at these mounting holes, the entire limit switch assembly detaches. Photos documented [here](https://github.com/abouzk/esp32-weld-motion-controller/tree/main/docs/audit).
      - Hazard: bracket detachment during carriage travel
      - Hazardous situation: loss of home position reference with no hardware stop signal
      - Harm: uncontrolled carriage travel beyond physical rail limits
      - Risk classification: High
      - Risk control measures: (1) firmware homing timeout as software detection layer, (2) mechanical bracket replacement as primary mitigation -- required before deployment
* **Next Action Items:**
  + Implement firmware with homing timeout as bracket-failure detection mechanism.
  + Coordinate limit switch bracket repair with machine shop before shop deployment.
  + Begin firmware Revision 1 development.

---

### 2026-02-26: Welder Selection Trade Study

* **Objective:** Select a MIG welder satisfying reproducibility, ease-of-use, and parameter capability requirements for the lab environment.
* **Architecture Decisions:**
  + System architecture constrained to inverter-based welding technology only
    - *Reasoning:* Technical consultation with welding SME confirmed that transformer-based machines fail the reproducibility requirement. Transformer machines are susceptible to voltage droop from wall-power fluctuations and analog wire-feed motor drift. Inverters use closed-loop microprocessor feedback to maintain mathematically flat arc voltage -- mandatory for reproducing consistent laboratory weld coupons across student cohorts.
  + Lincoln POWER MIG 211i selected over Miller Millermatic 211 and ESAB EM 210
    - *Reasoning:* Concept selection matrix evaluated three criteria -- Reproducibility, Ease of Use, Parameter Capability. Miller rejected on cost ($1,950). ESAB rejected on UI (basic interface fails ease-of-use requirement for novice operators). Lincoln passes all three criteria: full-color digital screen allows exact voltage and wire feed speed input per lab WPS without analog interpolation error. Arccaptain baseline rejected outright (two criteria failures: reproducibility and ease of use).
* **Blockers / Constraints:**
  + Lincoln unit ($1,600) exceeds proposed ~$500 budget. Per direct client instruction from Client Meeting #2, budget was explicitly removed as a constraint during this research phase. Budget will be reintroduced in a later project phase.
* **Next Action Items:**
  + Complete actuator characterization and microstepping analysis (March 18).
  + Finalize ESP32 + DM542T hardware configuration.

---

### 2026-02-10: Initial System Sketches & Hardware Exploration

* **Objective:** Establish a shared physical understanding of the system layout and identify subsystem boundaries for the electromechanical architecture.
* **Architecture Decisions:**
  + Three electrical domains defined: low-voltage logic (3.3V), motion drive (24V), welding power (220V)
    - *Reasoning:* Inverter welders produce broadband switching noise in the 20-100 kHz range. Without explicit domain isolation, this noise can couple into STEP/DIR signal lines via ground loops or inductive coupling, corrupting pulse timing and causing step loss. Domain separation is a primary EMI constraint, not a convenience.
  + `HOLD` state inserted between `WELDING` and `RETRACT` in state diagram
    - *Reasoning:* Automatic retract immediately after target distance is reached would drag the weld gun through the live molten pool if the operator has not yet released the arc. Operator confirmation input required before retract begins -- safety requirement, not a UX decision.
* **Blockers / Constraints:**
  + Hardware not yet assembled -- exploratory test welds conducted to observe human travel speed variability at 6-8 IPM before formalizing requirements. Confirmed that manual weld speed inconsistency is the primary source of bead non-uniformity in the current lab workflow.
* **Next Action Items:**
  + Formalize welder selection trade study with technical consultation.
  + Determine microcontroller and driver hardware.

---

### 2026-01-20: Project Kickoff & Requirements Definition (V-Model -- Requirements Phase)

* **Objective:** Establish system-level requirements for the automated weld travel subsystem from Voice of Customer (VoC) inputs provided by the faculty advisor and lab context.
* **Architecture Decisions:**
  + Subsystem boundary defined: motion control firmware does not control arc voltage, wire feed speed, or shielding gas
    - *Reasoning:* Applying NASA SE ConOps structure before any hardware selection forced explicit boundary definition. The welding process circuit (foot pedal to MIG welder) is fully independent of the motion control circuit. This boundary prevents scope creep and clarifies validation criteria -- the motion subsystem is validated against IPM accuracy only, not weld quality.
* **Blockers / Constraints:**
  + Requirements based on initial VoC only -- pending technical consultation with welding SME to validate IPM range and confirm inverter requirement.
* **Next Action Items:**
  + Conduct welder selection trade study with welding SME.
  + Characterize provided belt drive actuator against IPM requirements.

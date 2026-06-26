# Alpha HWR — BLE Bond Loss & Reconnect Design Notes

Fork of `eman/esphome-alpha-hwr` (@v0.4.0). Hardware: Seeed XIAO ESP32-S3 + W5500
Ethernet → Grundfos ALPHA HWR pump (BLE MAC `0C:4B:EE:11:B6:C9`), no WiFi/proxy.
ESPHome 2026.6.2. Pump has **outside-closet access only** — manual re-pairing is
hugely inconvenient, so the overriding goal is: **NEVER LOSE THE BOND once established.**

This file is the durable record of the diagnosis and the planned fix. It exists
because the reasoning was developed in a chat session that won't persist; the code
changes below should be read against this rationale. Future maintainers (including
future-me, and any Claude Code session, which cannot see the original chat) start here.

---

## 1. The Confirmed Failure Mechanism (bond loss on pump power-cycle)

Established by direct INFO-level log capture plus a `peer_bond_exists()` diagnostic
probe and Bluedroid `BT_BTM` stack lines (two independent confirmations).

The bond is lost through this exact chain:

1. Pump power-cycles; on return it **advertises / is connectable before its BLE
   stack is ready** to complete a security handshake.
2. ESP32's reconnect loop pounces and **requests encryption immediately** on
   connection-open (`esp_ble_set_encryption(..., ESP_BLE_SEC_ENCRYPT)`).
3. Pump not ready → **`0x61` Encryption Start Failed** on the FIRST attempt.
4. Bluedroid runs `btm_sec_clr_temp_auth_service()` and clears the device record's
   key — and this **propagates to flash** (the persisted NVS bond is destroyed,
   not merely the runtime record — see §2).
5. Now bondless: every subsequent reconnect sees `NO BOND` → requests pairing →
   pump (not in pairing mode) refuses with **`0x52` Pairing Not Supported** →
   infinite loop → permanent failure until a manual re-pair.

**Recovery hinges ENTIRELY on the first encryption attempt succeeding.** If the
first attempt lands when the pump IS ready, encryption succeeds and the bond is
never touched (observed: a clean reconnect was a single attempt, BONDED, ~168ms
auth). If the first attempt lands while the pump is NOT ready, the bond is gone.

It is a **race**: not deterministic. Same power-cycle test produced a clean 168ms
recovery once and a permanent failure another time. Verbose logging accidentally
"fixed" one run by slowing the cadence so the first attempt landed after the pump
was ready.

### Why retry-after-failure does NOT help
Once the `0x61` destroys the bond, there is no key left to retry with. By attempt #2
the ESP32 is a bondless device requesting pairing → `0x52`. Patience is useless
after the fact. The ONLY thing that matters is preventing / surviving that first
failure.

---

## 2. What Has Been Ruled Out (don't re-investigate these)

- **"Reload the bond from NVS after a failure" — NOT VIABLE.** A fresh ESP32 boot
  (which reloads bonds from flash into runtime records) reads `NO BOND` after the
  failure. So flash itself is wiped, not just the runtime device record. There is
  nothing in flash to reload. (This killed the cleanest hoped-for fix.)
- **Lowering `req_sec_level` (dropping MITM/bonding) — does NOT help.** Per ESP-IDF
  `btm_ble_link_sec_check` (btm_ble.c:1466), the ENCRYPT-vs-PAIR decision is
  `if (cur_sec_level >= req_sec_level) ENCRYPT else PAIR`. After the failure clears
  the key, `cur_sec_level` collapses to NONE, so NONE < anything → PAIR regardless
  of what we request. Lowering the requirement can't rescue a key that's already gone.
- **The bond is NOT being maliciously deleted by our code.** The destruction is
  Bluedroid clearing the device-record key on encryption failure. Our code's only
  role is *triggering* it by requesting encryption into a not-ready pump.

### Bond / pump facts confirmed
- The pump **retains its bond across power-cycle** in principle (phone reconnects
  with no re-pair; one ESP32 run reconnected BONDED in 168ms). The loss is the
  ESP32-side race, not a pump that forgets.
- Pump NVM durably retains schedule, flow target, speed setpoint, clock across
  power loss — only the BLE bond is fragile, and only via the ESP32-side `0x61`.

---

## 3. The Re-Pairing Race (why manual re-pair took 20-30 min)

Re-pairing is ALSO a timing race, same root cause (pump advertising before ready /
brief pairing-mode window). Confirmed by the success capture:

- Every FAILED attempt: ESP32 connects → Central-initiates pairing request →
  instant (~40ms) `0x52` rejection (pump not in pairing mode). The rejection causes
  disconnect + ~2.5s reconnect churn.
- The SUCCESSFUL attempt looked completely different:
  `BLE security request from device ... - accepting`  ← **PUMP initiated** (GAP
  `ESP_GAP_BLE_SEC_REQ_EVT`, handled at `ble_connection_manager.cpp:430`)
  → `BTM_SetEncryption busy, enqueue request` → `BT_SMP: FOR LE SC LTK IS USED` →
  `auth complete` → **`Auth mode: 0x09`** → `Pairing Status >> ON`. ~1 second total.

### Key insight: for THIS pump, a successful new bond is PUMP-INITIATED.
When the pump is genuinely in pairing mode, IT sends the Security Request. The
pump's request is both the "I'm in pairing mode NOW" signal and the trigger that
makes pairing work. The ESP32's eager Central-initiated request is
**counterproductive** during re-pair: it earns `0x52`, and the resulting churn
means the ESP32 is frequently mid-reconnect (disconnected) exactly when the pump
finally offers — so it misses the window. The ESP32 is "talking over" the pump.

The 20-30 min was waiting for the lucky alignment of (pump in pairing mode) AND
(ESP32 connected and quiet enough to catch the pump's offer).

**Operator signal:** the pump's front-panel connection-button light goes from
occasional-blink to SOLID ON when the bond completes — this is the in-closet
"it worked" indicator (no need to watch logs).

---

## 4. Chunk 1 Finding — the base does NOT auto-initiate encryption

Read of ESPHome 2026.6.2 base (`esp32_ble_client/ble_client_base.cpp`):
- Base exposes `pair()` → `esp_ble_set_encryption(ESP_BLE_SEC_ENCRYPT)` (line 148)
  as a *capability* but **never calls it** in the `ESP_GATTC_OPEN_EVT` path.
- The alpha-hwr `handle_connection_opened` (~line 203, "Requesting
  encryption/pairing") is the **SOLE trigger** of encryption.

**Therefore "who initiates" is fully controllable in the component.** Suppressing
the component's `set_encryption` call (in the re-pair case) lets the base sit quiet
and the pump initiate via `SEC_REQ_EVT` — no base override required.

- Base also tracks a `paired_` flag (`is_paired()`, true on auth success / false on
  disconnect) — usable by the readiness gate and status sensors.
- `ble_client_base.cpp:218` has a comment re: conn-param sensitivity "during
  pairing" — read fully when building Chunk 5.

Pairing *style* params (IO cap NONE = Just Works, `ESP_LE_AUTH_REQ_SC_BOND`,
key size 16) are set once in `init_security()` and are correct; they govern HOW
pairing happens, not WHO initiates.

---

## 5. The Fix Plan (phased; each chunk independently verifiable)

Ordered so the riskiest unknowns resolve before anything depends on them, and so
the single highest-leverage change (the readiness gate) lands early.

**Chunk 1 — DONE.** Verify base doesn't auto-initiate encryption. ✓ (see §4)

**Chunk 2 — Readiness gate (PRIMARY bond-preservation fix). IMPLEMENTED; Stage 1
validated. Stage 3 (the real payoff test) still pending.**

*Design:* don't request encryption immediately on connection-open. Wait until the
pump proves ready — use **successful unencrypted service discovery** as the
readiness signal — then request encryption. Prevents the `0x61` that destroys the
bond. This is THE fix; everything else makes it more robust or less painful when it
slips. (Empirical readiness signal preferred over a fixed settle-delay, which is a
guess. Confirmed safe: tonight's logs show discovery + CCCD writes succeed
UNENCRYPTED on this pump, so discovery-first is viable.)

*What was actually changed (Option A — minimal; chose A over fuller "B" that would
also serialize subscribe/auth, to avoid disturbing the working reconnect chain):*
- `ble_connection_manager.h`: added one member `bool encryption_requested_{false}`
  (guards against >1 encryption request per connection, since the discovery-complete
  handler can re-enter via the retry path). Reset false on each connection-open.
- `handle_connection_opened`: REMOVED the immediate
  `esp_ble_set_encryption(..., ESP_BLE_SEC_ENCRYPT)` block. Everything else
  (callbacks, conn-params, the POST_CONNECT_DELAY_MS discovery-stabilize timer, the
  `peer_bond_exists()` diagnostic log) unchanged.
- `handle_service_discovery_complete`: ADDED the encryption request right after the
  service is confirmed found, before `subscribe_to_notifications()`, guarded by
  `if (pairing_enabled_ && !encryption_requested_)`. Logs
  "Pump ready (discovery ok). Requesting encryption/pairing...".
- The subscribe/auth chain was deliberately NOT reordered (that's the "B" refinement,
  deferred). encryption + subscription now run concurrently after discovery — proven
  safe by tonight's logs where subscription (CCCD writes) succeeded while encryption
  was still resolving. Encryption call now exists in EXACTLY ONE place (the
  discovery-complete handler) — so the ordering flip is guaranteed by construction,
  not by runtime timing.
- The encryption request is intentionally NOT branched on bonded-vs-no-bond: the same
  `esp_ble_set_encryption(ESP_BLE_SEC_ENCRYPT)` call serves both (stack routes to
  encrypt-with-bond or pair based on bond state). Keeps Chunk 2 minimal; the
  bonded-vs-pairing *behavioral* split is Chunk 5/6's job.

*Testing (staircase, safest first):*
- Stage 0 (compile/boot) — PASS (compile timestamp confirmed).
- Stage 1 (normal bonded reconnect still works — the safety test; ESP32 reboot, pump
  untouched, ZERO bond-loss risk because a READY pump never produces the 0x61) —
  PASS on behavioral evidence: after OTA reboot AND after a deliberate restart, the
  device reconnected BONDED to READY with full clean telemetry (1648-1653 RPM, 2.3 W,
  no alarms, Constant Speed @ 1650). A successful bonded reconnect REQUIRES encryption
  to have fired and succeeded, on exactly the path Chunk 2 modifies → proves the
  relocated call works and the working path is intact.
- NOTE on why we never *saw* the ordering flip in a capture: the ESPHome API log
  stream rides the network link to the ESP32 and DIES when the ESP32 reboots; it
  re-attaches only after the fast BLE reconnect already happened. So a reboot-
  triggered BLE sequence is structurally uncatchable over the API logger. Serial/USB
  logging would catch it (survives reboot) but needs closet access and only shows the
  SAFE path anyway — not worth a trip. The single-call-site structure already
  guarantees the flip.
- Stage 3 (THE payoff test — does the gate actually prevent bond loss when the PUMP
  is not ready) — **3/3 PASSES (2026-06-26 09:01, 09:17, 09:25), all captured over
  network.** Each: pump off ~2 min, then on. Every run: first reconnect `Bond state:
  BONDED` (bond SURVIVED) → `Service discovery complete` → `Pump ready (discovery ok).
  Requesting encryption` (gate engaged: encryption AFTER discovery) → `Auth mode:
  0x09` → READY. **NO `0x61`, NO `btm_sec_clr_temp_auth`, NO `0x52` in any run** — the
  bond-destruction signature is ABSENT across all three. Run 2 (09:17) is the
  strongest: TWO connection-opens before completion, BONDED on BOTH — the gate held
  the bond through a bumpy multi-attempt reconnect that would have killed the old
  code. Reconnects are now instant, no loop.
  EPISTEMIC STATUS: this is a strong, repeated, mechanism-verified result against a
  RACE condition — three independent cold power-cycles, mechanism visible each time.
  That is about as much confidence as empirical testing of a race can give short of
  indefinite runs. NOT claimed as absolute proof (a race can win on its own), but the
  pattern is behavior, not luck. **Chunk 2 readiness gate VALIDATED for the
  real-world pump-power-blip case.** Continue to watch real power events as they
  happen; treat each clean reconnect as one more data point.
  OPEN COUNTEREXAMPLE (keep watching): the bond WAS lost earlier the same morning
  (~08:09) during a DIRTY DUAL-POWER transition (PoE dropped while USB connected →
  brownout limbo, NOT a clean pump-cycle). That loss was UNCAPTURED (cause unproven:
  dirty-transition corruption vs. an uncaptured 0x61 vs. NVS corruption). It is a
  DIFFERENT event class than the 3 clean passes (ESP32 disturbed, not just
  pump-not-ready). Provisionally attributed to the dual-power mishap; watch whether
  anything similar recurs on a CLEAN reboot. If it does, there is a second failure
  mode the gate doesn't cover. If it never recurs on clean reboots, it was the
  self-inflicted power mess.
- Stage 1 ORDERING FLIP CONFIRMED OBSERVED (this morning, via an unplanned reconnect):
  log showed `Bond state: NO BOND → Starting service discovery → Service discovery
  complete → Pump ready (discovery ok). Requesting encryption/pairing` — encryption
  fired AFTER discovery, exactly as designed. Gate mechanism is now observed, not just
  inferred from the single-call-site structure.

**HARDWARE / POWER HAZARD (learned the hard way this morning):**
This board has BOTH PoE and (when plugged for serial) USB power. Dropping ONE rail
while the other is connected does NOT cleanly power-cycle — the ESP32 FAILS OVER to
the other source, producing a dirty brownout/partial-reset in limbo rather than a
clean off/on. This morning, dropping PoE with USB connected caused exactly this, and
the BLE bond was LOST during the uncaptured transition (cause unconfirmed — could be
the dirty transition corrupting state, or a 0x61 reconnect that wasn't captured;
logging wasn't running through the event so we cannot prove which). LESSON:
- To reboot cleanly: use the **RESET button** (no power transition at all) or the
  **HA/API restart** — NOT rail-dropping.
- Never power-cycle by dropping one source while the other is connected.
- Note ESP32-reboot-toward-a-READY-pump has historically been the SAFE case (dozens
  of clean reboots reconnected fine); the dirty dual-power transition is a NEW,
  self-inflicted failure mode, distinct from the pump-not-ready 0x61 race.

**METHODOLOGICAL NOTE — capturing the destroying cycle:** every bond loss so far
happened during an UNCAPTURED moment (OTA reboot, power mishap), so we keep seeing
the aftermath (NO BOND loop) never the act. To settle "does the gate prevent loss":
logging MUST run BEFORE the trigger. Pump power-cycle (Stage 3) is capturable over
network (ESP32 stays up). ESP32-reboot scenarios are only capturable over USB serial
(survives reboot) AND require `hardware_uart: USB_CDC` in the logger config first —
the default routes full logging to UART0, not the USB-CDC port, so USB shows only a
sparse partial stream until that's set.

**Chunk 3 — Investigate IDF "don't auto-clear bond on failure" knob (research).**
Before building the complex backstop, check whether ESP-IDF can be told not to
destroy the bond on a transient encryption failure. If such a knob exists it may
make Chunk 4 unnecessary — structurally simpler than snapshot/restore.

**Chunk 4 — Bond key snapshot/restore (SURVIVAL backstop). Only if Chunk 3 finds
nothing.** Snapshot bond keys to our own NVS at pairing-success; re-inject if the
bond is ever unexpectedly gone, so the ESP32 self-heals without a re-pair.
Higher-risk / IDF-version-sensitive — justified ONLY by the 30-min re-pair cost,
and only if prevention can't be made airtight.

**Chunk 5 — Patient re-pairing mode (fixes the re-pair race).**

DIAGNOSIS SHARPENED (2 confirmed pairing captures): the re-pair timing is NOT the
pump failing to enter/stay in pairing mode — the pump offers RELIABLY. The problem
is the **ESP32 being intermittently un-receptive when the pump offers.** Its ~2s
connect → fail(0x52) → reconnect loop means that when the pump sends its Security
Request, the ESP32 may be mid-reconnect, or just fired its own premature
Central-initiated request, i.e. "talking" (and sending garbage that may disrupt the
pump's pairing offer) when it should be "listening." Whether the pump's offer lands
in a receptive moment is LUCK (yesterday 20-30 min unlucky; today ~2 min lucky).

Both successful pairings (last night + this morning 08:39:56) followed the SAME
pattern: a string of ~1.7s-spaced `0x52` rejections while the ESP32 asks and the
pump isn't offering, then the instant the PUMP sends
`BLE security request from device ... - accepting` (GAP SEC_REQ_EVT,
`ble_connection_manager.cpp:444`), pairing completes in ~1s → `Auth mode: 0x09`.
**For this pump a new bond is PUMP-INITIATED; the ESP32's own requests only generate
0x52 noise until the pump offers.** The readiness gate did NOT interfere with pairing
(discovery → pump security request → 0x09 flowed cleanly).

GOAL: get luck out of the equation. Chunk 5 must make the ESP32 DETERMINISTICALLY
receptive — connect, go quiet (do NOT fire Central-initiated pairing requests that
earn 0x52 and may disrupt the pump's offer), and HOLD the connection open in a
listening state so that whenever the pump offers its Security Request, the ESP32 is
always ready to accept. Converts pairing from a timing lottery into
"pump offers → ESP32 accepts," every time. Enabled by Chunk 1's finding (base won't
auto-initiate, so the component can stay quiet and just handle SEC_REQ_EVT).
Open implementation question: also suppress/slow the connect-fail-reconnect churn
during re-pair so the ESP32 isn't repeatedly dropping the connection the pump is
trying to offer on (hold one connection open rather than churning).

**Chunk 6 — Operator surface (HA-facing).** Mechanical once §5 mechanism works:
- `repair_requested` flag: persisted (NVS), reversible, auto-clears on pairing
  success. Authorizes pairing only when a bond already exists.
- Decision logic at connection-open:
  `no stored bond → pair (fresh bring-up); bond + repair_requested → pair;
   bond + no request → reconnect/encrypt-only forever (NEVER auto-escalate).`
- Buttons: keep **Clear Pump BLE Bond** (wipe bond → next connect auto-pairs via
  no-bond path); add **Re-Pair Pump** as a reversible TOGGLE (sets repair_requested,
  leaves bond intact, auto-off on success, manually cancelable). When Re-Pair on →
  ONLY pair (no bonded-reconnect fallback; honor operator intent).
- Sensors (three, distinct jobs):
  - `Pairing Status` — existing binary primitive, untouched.
  - `Pump Link Status` — coarse, action-mapped enum: `connected` / `reconnecting`
    / `pump_unreachable` (won't even open = no power) / `bond_failed` (opens but
    encryption fails) / `pairing` / `unpaired` (no bond, not trying). Each maps to a
    distinct physical action for pyscript notifications.
  - `Pairing Detail` — active-attempt forensics only: `idle` / `key_exchange` /
    `awaiting_pump_pairing_mode` (the `0x52` "go press the pump button" case) /
    `pairing_failed_security`. NO `paired` state; returns to `idle` when no attempt
    active.
- pyscript watchdog notification keyed off `Pump Link Status` transitions (reuse
  `send_notification_once` cooldown).

---

## 6. Diagnostic scaffolding currently in the build (Pass 1)

Pure-diagnostic, no behavior change — keep or quiet later:
- `bool peer_bond_exists()` in `ble_connection_manager` — queries
  `esp_ble_get_bond_device_num/_list`, compares to `client_->get_remote_bda()`.
- Logs "Bond state at connection open: BONDED/NO BOND" in `handle_connection_opened`.
- Logs "Bond present after failure: YES/NO" in `handle_auth_complete` failure branch.
- (Chunk 2) Logs "Pump ready (discovery ok). Requesting encryption/pairing..." in
  `handle_service_discovery_complete` — the marker that the readiness gate fired; its
  position AFTER "Service discovery complete" is the visible proof the gate engaged.
Recommendation: KEEP — cheap and was invaluable for the whole diagnosis.

Quick log-verification greps (PowerShell):
- Connection sequence / gate ordering:
  `Select-String -Path <log> -Pattern "Bond state at connection open|Service discovery complete|Pump ready|Requesting encryption|Auth mode|0x61|0x52|Session:.*READY"`
- A clean reconnect should read, in order: BONDED -> Service discovery complete ->
  Pump ready (discovery ok). Requesting encryption -> Auth mode: 0x09 ->
  Session: AUTHENTICATING -> READY, with NO 0x61.

---

## 7. Verified base API (ESPHome 2026.6.2)

- `parent_->disconnect()`, `parent_->unconditional_disconnect()`
- `parent_->connected()` == `state() == ESTABLISHED`; `state()`/`ClientState`
- `set_auto_connect(bool)` ; base drives the ~2.5s reconnect loop (no YAML reconnect
  option is set, so stock auto_connect is active)
- `is_paired()` / `paired_` flag (true on auth success, false on disconnect)
- Reconnect loop lives in the base, NOT in the component → backoff must be expressed
  as a scheduled delay before requesting encryption in `handle_connection_opened`
  (the component already has `scheduler_callback_` → parent `set_timeout`), not as a
  loop edit.

---

## 8. Reference resources

- ESP-IDF v5.3 Bluedroid: `btm_ble.c` `btm_ble_link_sec_check` (line 1466, the
  ENCRYPT-vs-PAIR decision); `btm_sec.c` `btm_sec_clr_temp_auth_service` (line 803,
  the clear-on-failure path); `smp_act.c`.
- christoph2/GENIBus — reverse-engineered GENIbus data-item tables.
- GENIbus Protocol Spec (Aug 2005), Ch.4 = scaling.

---

## 9. Other deferred work (independent of BLE)

- **pyscript `verify_pump_config` retry fix** — verify-and-retry loop for the
  schedule-disable BLE write (designed, deploy not confirmed).
- **Telemetry scaling** in `telemetry_decoder.cpp`: flow ~4.1x high, power ~6x high
  (matters for wattage averaging), flow setpoint ~162x off. Constant-Speed setpoint
  readback is clean. Needs same-instant app-vs-component capture of flow/power/RPM
  (RPM is the anchor).
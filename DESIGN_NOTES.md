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

**Chunk 2 — Readiness gate (PRIMARY bond-preservation fix).**
In `handle_connection_opened`, for a *bonded reconnect*, do NOT request encryption
immediately. Wait until the pump proves ready — use **successful unencrypted
service discovery** as the readiness signal — then request encryption. This
prevents the `0x61` that destroys the bond. This is THE fix; everything else makes
it more robust or less painful when it slips. (Empirical readiness signal preferred
over a fixed settle-delay, which is a guess.)

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
When re-pairing (no bond, or Re-Pair requested): the ESP32 connects and **waits for
the pump's Security Request** instead of firing premature Central-initiated
requests. Catch the pump's offer instead of talking over it. Turns the 20-30 min
alignment lottery into "press pump button → ESP32 catches it promptly." Enabled by
Chunk 1's finding (base won't auto-initiate, so the component can stay quiet).

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
Recommendation: KEEP — cheap and was invaluable for the whole diagnosis.

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

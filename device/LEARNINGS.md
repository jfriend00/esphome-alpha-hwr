# Codebase & Device Learnings

A topic-indexed reference of durable facts discovered while working on this fork — about
the pump's firmware behavior, the ESP-IDF / Bluedroid BLE stack, the ESPHome `ble_client`
base, and this component.

Unlike `DESIGN_NOTES.md` (the chronological narrative of the bond-loss investigation),
this file is for **atomic, lookup-oriented facts** that outlive whatever investigation
surfaced them. Each entry: the fact, a short explanation, a code/source pointer, and —
where a fuller story exists — a pointer to `DESIGN_NOTES.md` rather than a copy of it.

Domains: **Pump (firmware) behavior** · **ESP-IDF / Bluedroid** ·
**ESPHome `ble_client` base** · **This component**, plus a **BLE code cheat-sheet** and
**Known unknowns**.

---

## Pump (firmware) behavior

### The pump enforces a ~430ms post-discovery encryption-start deadline

After GATT service discovery completes, the pump drops an idle, discovered-but-
unencrypted connection roughly **430ms** later with a `0x13` (remote-user-terminated)
disconnect — i.e. it requires encryption to *begin* within ~430ms of discovery. This
holds whether or not a bond exists (measured 405–458ms across five consecutive no-bond
cycles). An active pairing handshake pre-empts it: once the pump sends its `SEC_REQ`, the
link stays up through pairing (observed ~5.4s to READY).

Consequences:
- Any "settle delay" between discovery and the encryption request is incompatible with
  this pump (even 500ms exceeds the deadline).
- In the no-bond re-pair state, the device cannot hold one connection open and wait; the
  pump tears it down every ~430ms, producing a ~2.2s clean reconnect cycle. That cycling
  *is* the patient-listen mechanism, not a bug.

Source: measured from captured logs; full narrative in `DESIGN_NOTES.md` §5 (Chunk 2.5
and the Test B results).

### A new bond is pump-initiated (the pump sends the SEC_REQ)

This pump only completes a *new* bond when it initiates pairing: when it is in pairing
mode it sends the BLE security request (`SEC_REQ`), which both signals "I'm pairing now"
and triggers the handshake. A central-initiated pairing request (the ESP32 calling
`esp_ble_set_encryption` with no bond) is rejected with `0x52` (pairing-not-supported)
and just produces reconnect churn that competes with the pump's offer. Re-encryption
against an *existing* bond, by contrast, is central-initiated and normal.

Source: confirmed across multiple pairing captures; `DESIGN_NOTES.md` §3 and §5
(Chunk 5). Drives the no-bond "quiet-listen" behavior.

### The pump is connectable before it is key-ready after power-up

On return from a power-cycle the pump advertises and accepts a GATT connection — and even
service discovery and CCCD writes — *before* it has loaded its bond keys from its own NVM.
Requesting encryption in that window makes the pump report it has no key (`KEY_MISSING`),
the attempt fails (`0x61`), and the local bond is cleared (see ESP-IDF section) → manual
re-pair. The window is a few seconds; service discovery (~1.5–2s) usually outlasts it,
which is why gating encryption on discovery-complete avoids it.

Source: `DESIGN_NOTES.md` §1 and §5. Root cause behind the readiness gate.

### The pump requires encryption before notification subscription

The pump rejects an unencrypted CCCD write (notification-enable) with a `0x13` disconnect;
the encrypted link must exist before subscribing. The current bonded-reconnect path issues
encryption and the CCCD write *concurrently* and wins by timing rather than enforcing the
order — a known limitation (see `UPSTREAM_NOTES.md` §3 / `DESIGN_NOTES.md` race #2).

Source: `DESIGN_NOTES.md` §5 (Chunk 2.5, race #2).

### This pump's minimum speed is ~1660 RPM (lower commands clamp to it)

Commanding a constant-speed setpoint below ~1660 RPM has no effect — the pump still runs
at 1660, so that's the slowest it will actually go. This is why 1660 is the chosen "slow"
value (and `FALLBACK_SPEED_RPM = 1660`): it's the floor, not an arbitrary number. Likely
pump/model-specific — a fact about *this* unit, not the component; other ALPHA HWR units
may have a different floor.

Source: observed on the device.

---

## ESP-IDF / Bluedroid

### Bluedroid clears the bond key on encryption failure — unconditional, no knob

When an LE encryption/auth completes with `HCI_ERR_KEY_MISSING`, `HCI_ERR_AUTH_FAILURE`,
or `HCI_ERR_ENCRY_MODE_NOT_ACCEPTABLE`, Bluedroid clears the device record's LE link-key
flags (`btm_sec.c`, ~line 4133 in ESP-IDF v5.3: clears `BTM_SEC_LE_LINK_KEY_KNOWN`, sets
`key_type = BTM_LE_KEY_NONE`). This is unconditional for BLE — there is no config/sdkconfig
option to disable it, and there is **no public API to write a key back**. This is the
mechanism that destroys the bond when encryption is attempted into a not-ready pump, and
the reason prevention (the readiness gate) is the only practical fix.

Red herring to ignore: the `btm_sec_clr_temp_auth_service()` log line clears a temporary
GATT service-authorization flag, **not** the bond key.

Source: ESP-IDF v5.3 Bluedroid `btm_sec.c`; `DESIGN_NOTES.md` §5 (Chunk 3).

### esp_ble_remove_bond_device() on a live connection drops the link

Calling `esp_ble_remove_bond_device()` for the currently-connected peer terminates the
connection locally — observed as a `0x16` (terminated-by-local-host) disconnect ~90ms
after the call. Practical upshot: the "Clear Pump BLE Bond" control both wipes the local
bond *and* forces a reconnect (into the no-bond pairing path) as a side effect; no separate
disconnect step is needed.

Source: observed in capture (`chunk5-3.log`); `DESIGN_NOTES.md` §5 (Test B).

### esp_ble_gap_set_security_param() takes length 1 for these params, not sizeof(enum)

The IO-cap, auth-req, and key-size security params are passed with `sizeof(uint8_t)`
(length 1) even though the value sits in a typed local (e.g. `esp_ble_io_cap_t`). The API
treats them as single-byte values; on the little-endian ESP32 the low byte carries the
value, and this matches the official ESP-IDF `gatt_security_*` examples. Passing
`sizeof(esp_ble_io_cap_t)` (4) is wrong — it diverges from the examples and copies extra
bytes.

Source: `init_security()` in `ble_connection_manager.cpp`; ESP-IDF security examples.

---

## ESPHome `ble_client` base

### The base never auto-initiates encryption

The ESPHome 2026.6.2 base exposes `pair()` → `esp_ble_set_encryption(...)` as a capability
but never calls it in the `ESP_GATTC_OPEN_EVT` path. The alpha_hwr component is the **sole**
trigger of encryption. This is what makes the no-bond "quiet-listen" strategy possible:
suppressing the component's request genuinely leaves the link silent for the pump to drive
— no base override required.

Source: `esp32_ble_client/ble_client_base.cpp`; `DESIGN_NOTES.md` §4 (Chunk 1).

### Service discovery is base-driven and fires immediately on connection-open

The base calls `esp_ble_gattc_search_service()` directly in its `ESP_GATTC_OPEN_EVT`
handler, so discovery starts the instant the connection opens — the component cannot
postpone it. Consequences: a "wait before discovery" settle delay is impossible, and the
component's `POST_CONNECT_DELAY_MS` timer gates nothing real (see component section).

Source: `esp32_ble_client/ble_client_base.cpp`; `DESIGN_NOTES.md` §5 (Chunk 2.5).

---

## This component

### "Pairing Status" binary sensor = latched outcome of the last BLE auth-complete

Updated in exactly two places, both in `handle_auth_complete()`
(`ble_connection_manager.cpp:348` / `:404`):
- **ON** when a BLE security handshake completes successfully — encryption against an
  existing bond (normal reconnect) or a new pairing (`Auth mode: 0x09`).
- **OFF** when one fails (e.g. `0x52` pairing-not-supported, `0x61` encryption-start
  failure).

What it is **not**:
- Not the GENI application authentication (the 3-stage handshake that drives the session
  to READY and starts telemetry) — that never touches this sensor.
- Not a live-connection indicator: nothing sets it OFF on disconnect, so it **latches**
  its last value (stays ON after a successful auth followed by a later disconnect).
- Not updated during no-bond re-pair cycling (those end in `0x13` disconnects, not
  auth-complete events).
- "Unknown" until the first auth-complete after boot.

Read it as "the last BLE encryption/pairing attempt succeeded," **not** "currently
paired and connected."

Source: `ble_connection_manager.cpp` `handle_auth_complete()`; sensor wired in
`__init__.py` (`CONF_PAIRING_STATUS`) and `packages/alpha_hwr_pairing.yaml`.

### POST_CONNECT_DELAY_MS (500ms) only delays a log line, not discovery

`handle_connection_opened()` schedules a `POST_CONNECT_DELAY_MS` (500ms) timer whose
callback merely logs "Starting service discovery...". Because discovery is base-driven and
already started on OPEN (see base section), this timer gates nothing real — it is
effectively cosmetic/dead. Don't mistake it for a stabilization delay.

Source: `handle_connection_opened()` in `ble_connection_manager.cpp`; `DESIGN_NOTES.md`
§5 (Chunk 2.5).

### peer_bond_exists() is an address-match and is load-bearing for the encrypt-vs-wait decision

`peer_bond_exists()` queries the ESP-IDF bond store and compares each stored bond's
identity address to the connected peer's address. Since the pump-initiated-pairing change
it *decides* the connection's path (bond present → request encryption; no bond →
quiet-listen for pump-initiated pairing), not just diagnostics. It is sound here only
because the component connects by a **fixed MAC** (no RPA rotation), so the connection
address always equals the bonded identity address. It also handles **pump replacement**
correctly: a stale bond for the *old* MAC won't match the *new* configured MAC → reads
NO BOND → re-pairs. A bond-*count* check would get replacement wrong.

Source: `peer_bond_exists()` in `ble_connection_manager.cpp`; `DESIGN_NOTES.md` §5
(Chunk 5).

### Pump control: on/off, mode, and setpoint all funnel through one Class 10 object

Start / stop / set-mode are a single Class 10 write — OpSpec `0x90` (SET), Sub `0x5600`,
Obj `0x0601` — with a 12-byte payload:

    2F 01 00 00 07 00 [Flag] [ModeByte] [Suffix(4)]
      Flag:   0x00 = start/run, 0x01 = stop
      Suffix: float32-BE setpoint if supplied, else the mode's default suffix

What follows from this:
- **No standalone on/off in this implementation.** Every start/stop also carries the mode
  byte and a mandatory 4-byte suffix. (Stop doesn't need a *real* setpoint — it sends the
  mode's default suffix — but the field is always present and you must know the current
  mode.) Whether the pump firmware offers a simpler on/off via another GENIbus class is an
  open question — see `device/ARCHITECTURE_NOTES.md`.
- **A separate setpoint-only write exists**: OpSpec `0x84`, Obj 86, Sub 13 (speed) /
  15 (pressure) / 39 (flow), float32-BE, carrying **no run-state**.
- **Setting speed turns the pump on** because the set-speed routine leads with the
  control-object write (Flag=Start) and *then* writes the dedicated setpoint. That coupling
  is an implementation choice — the setpoint-only write above could in principle change
  speed without starting (untested while stopped).
- **No setpoint passed → the mode's default suffix is sent**, so stock turn-on ignores the
  configured setpoint and runs at that default. (In this fork the constant-speed default
  suffix is `44 CF 80 00` = 1660 RPM — *John's edit*; upstream's was `45 65 70 00`
  ≈ 3670 RPM.) The `local:` patch fixes this by injecting the HA value on start.

Other control ops: read mode/op/setpoint = OpSpec `0x03`, Obj 86 Sub 6 (→ notification
Obj `0x2F01` Sub 1); enable / disable remote = Class 3 `03 C1 07` / `03 C1 06` (remote-vs-
auto, **not** pump on/off); temp-range / cycle-time configs = Class 10 Obj 91 Sub 430.

Source: `control_service.cpp` (`send_control_request`, `set_class10_setpoint`, `start`).
(The `.h` doc-comments mention a Class 3 register method for setpoints; the `.cpp` actually
uses Class 10 OpSpec `0x84` — trust the `.cpp`.)

### Non-speed setpoints are unit-broken; constant-speed/RPM is the reliable path

The flow and pressure setpoints come back garbage / with unit-conversion errors (part of
the broader telemetry-scaling problems — see `DESIGN_NOTES.md` §9). Constant-speed (RPM)
was chosen for control here precisely because RPM is effectively **unitless** — no Pa↔m or
scaling factor to get wrong — so it's the one mode that works cleanly today.

### Speed control is an HA-as-master divergence (usage-model choice)

The control object makes the pump the master of the intended speed (you set it; it's
stored and re-sent on start). That's *required* if you use the pump's built-in scheduler
(it turns on autonomously and must know its speed) — the phone-app / autonomous-operation
use case upstream targets. This setup is HA-driven instead, so a `local:` patch makes **HA**
the master (keep desired speed in HA, inject on every turn-on), which also survives pump
resets/replacements. Stays personal until/unless the component supports both models. See
`GIT_WORKFLOW.md` §1, §8 and `device/ARCHITECTURE_NOTES.md`.

Source: `control_service.cpp` `start()` / `send_control_request()`.

---

## BLE code cheat-sheet

Codes as they appear in this project's logs (ESP-IDF Bluedroid). `0x61` and `0x52` are
**auth-complete failure reasons** (the "BLE authentication failed / Failure reason" lines);
`0x08` / `0x13` / `0x16` are **disconnect reasons** (the "Disconnected (reason: …)" lines).

| Code | Meaning | Typically seen when |
|------|---------|---------------------|
| `0x61` | Encryption start failed | Encryption requested into a not-ready pump — **clears the bond** |
| `0x52` | Pairing not supported | Central-initiated pairing while the pump isn't in pairing mode (no bond) |
| `0x13` | Remote user terminated connection | Pump drops the link: subscribe-before-encrypt, idle no-bond at ~430ms, or out-of-order ops |
| `0x08` | Connection timeout (supervision) | RF loss, or the first not-ready connection-open after power-up |
| `0x16` | Terminated by local host | e.g. `esp_ble_remove_bond_device()` on a live link, or a local `disconnect()` |
| `Auth mode 0x09` | Secure Connections + bonding, no MITM (success) | A successful pairing / encryption auth-complete |

Note: `0x13` has several distinct causes here — don't assume which without surrounding
context.

---

## Known unknowns

Open questions without a confirmed answer — recorded so they aren't mistaken for settled
facts.

- **Pump BLE bond-slot capacity and eviction policy.** We assume re-pairing the same
  identity *replaces* the pump's bond (so slots don't accumulate), but the pump's
  stored-bond limit and its behavior when full are unverified. Backstop if it ever fills:
  a pump factory reset. (See `device/TODO.md`.)
- **Whether other ALPHA HWR units are also pump-initiated.** Confirmed on one unit; we
  can't verify other units / firmware revisions don't expect central-initiated pairing.
  This is the open question flagged for the upstream PR (`UPSTREAM_NOTES.md` §2).
- **The post-pairing resume hook is unexercised.** `handle_auth_complete` → subscribe
  hasn't been hit in capture because pairing completes before discovery on this pump;
  correct as insurance but unvalidated. (See `device/TODO.md`.)
- **Whether the setpoint can be changed without starting the pump.** We now know *why*
  setting speed starts it — the set-speed routine leads with the start-asserting control
  write — and that a setpoint-only write exists (Class 10 OpSpec `0x84`, Sub 13, no
  run-state). Unverified: whether a standalone setpoint-only write *while stopped* sets the
  active value. (See the control-commands entry above and `device/ARCHITECTURE_NOTES.md`.)
- **Whether the pump supports a simpler GENIbus Class 3 on/off command.** On/off is a
  standard GENIbus command; the code already uses Class 3 for remote enable/disable
  (`03 C1 07` / `06`), so the pump accepts Class 3 command IDs. Whether a bare
  `03 C1 <id>` start/stop works needs an experiment on the device. (See
  `device/ARCHITECTURE_NOTES.md`.)

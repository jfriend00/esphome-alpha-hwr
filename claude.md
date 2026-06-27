# esphome-alpha-hwr (John's fork)

Controlling a Grundfos ALPHA HWR recirc pump via BLE on a Seeed XIAO ESP32-S3
+ W5500 Ethernet, integrated with Home Assistant. Forked from eman/esphome-alpha-hwr.

## READ FIRST
- `device/DESIGN_NOTES.md` — the authoritative record of the BLE bond-loss
  investigation, the readiness-gate fix, dead ends (settle delay, Option B),
  and the remaining plan (Chunks 5-6). Read it before touching BLE code.

## Current state
- Chunk 5 (patient re-pairing) IMPLEMENTED and VALIDATED end-to-end — Tests A/B/C all
  pass (2026-06-26). Built on the Chunk 2 readiness gate (bond-loss-on-pump-power-cycle
  FIXED, Stage 3 3/3). No-bond connections now quiet-listen (no `0x52`) and catch the
  pump's pump-initiated `SEC_REQ`; bonded reconnects unchanged. See DESIGN_NOTES Chunk 5.
- Race #2 (operation-ordering `0x13` on bonded reconnect) is KNOWN, mitigated (no bond
  loss), NOT fixed. The Chunk 5 resume hook is correct-but-unexercised insurance. See
  `device/TODO.md`.
- Settle delay + an earlier Option B were tried and REVERTED (see DESIGN_NOTES — dead ends).
- Tag: `before-adding-encryption-settle-delay` is the known-good rollback point.

## Build / test
- Build/flash from `device/`: `esphome run recirc-controller.yaml`
- Device IP: 10.10.3.187 (OTA over Ethernet, no WiFi/proxy)
- Capture logs: `esphome logs recirc-controller.yaml 2>&1 | tee stageN.log`

## Hard-won constraints (DO NOT relearn the hard way)
- Pump enforces a ~430ms encryption-start deadline after discovery; settle delays
  are incompatible. Don't reintroduce them.
- Bond loss historically = pump-power-cycle encryption race (KEY_MISSING). The gate
  fixes it. Don't request encryption before service discovery completes.
- Re-pairing is PUMP-INITIATED; the ESP32 must wait for the pump's security request.
- API logger drops on ESP32 reboot — reboot-triggered BLE sequences are uncapturable
  over API; pump-power-cycle sequences ARE capturable (ESP32 stays up).
- Minimize OTA reflashing; batch changes (each reflash is an uncaptured reboot gap).

## My working style
- Small, independent, verified changes over big batches. Complete files/functions,
  not fragments. Empirical verification ("let the data tell you") over theory.
- Push back when I'm wrong; don't over-assert past the evidence.
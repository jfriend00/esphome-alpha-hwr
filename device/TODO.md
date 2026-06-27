# Alpha HWR — Running TODO / Backlog

Lightweight tracker for small fixes, deferred decisions, and open questions that
come up during development, so they don't get lost. Big design narratives live in
`DESIGN_NOTES.md`; larger deferred work is tracked in `DESIGN_NOTES.md §9`. This
file is for the "don't forget this" items.

Status key: `[ ]` open · `[~]` deferred (waiting on data/decision) · `[x]` done

---

## BLE bond / pairing

- [ ] **Stale-bond accumulation on pump replacement.** When a pump is replaced,
  the YAML MAC changes but the OLD pump's bond stays in NVS forever. Harmless for
  the Chunk 5 decision (it's filtered by address in `peer_bond_exists()`), but
  bonds accumulate and ESP-IDF caps stored bonds (`BLE_MAX_BONDS`, often ~4 by
  default). After several replacements the store could fill.
  - *Options:* use the existing **Clear Pump BLE Bond** button at replacement time;
    or auto-prune any bond whose MAC ≠ the configured pump MAC.
  - *Priority:* low. *When:* before this sees multiple pump generations. Likely a
    Chunk 6 (operator-surface) concern.

- [x] **"Awaiting pairing" liveness — RESOLVED for this pump (no watchdog needed).**
  Test B (`chunk5-3.log`, 2026-06-26) settled it: the pump reliably drops an idle,
  discovered, unencrypted no-bond connection at **~430ms** (`0x13`), giving a fresh
  clean ~430ms listening window every ~2.2s with zero `0x52` noise. The feared
  "pump holds the link open forever, only a reboot escapes" does NOT happen — the
  pump never holds it open. So the natural cycling IS the patient-listen mechanism;
  no internal watchdog required.
  - *Residual (low):* a watchdog (`awaiting_pump_pairing_` for N s with no `SEC_REQ`
    -> `parent_->disconnect()`) survives only as hypothetical insurance for a future
    NEVER-BONDED replacement pump that might idle differently (Test B had the pump
    still bonded on its side = real post-loss state, not a virgin pump). Build only
    if such a pump ever shows the hold-open behavior.

- [x] **Chunk 5 reconnect-churn suppression — RESOLVED: do NOT suppress.** Test B
  showed the churn is pump-driven (~430ms `0x13` drop), clean (no `0x52`/`0x61`), and
  is exactly the desired patient-listen mechanism — a fresh window every ~2.2s. There
  is nothing to suppress; suppressing it would remove listening windows. Closed.

- [ ] **Chunk 5 resume-subscribe hook is UNEXERCISED (pairing-before-discovery
  dominates).** In Test C (`chunk5-4.log`, 2026-06-26) pairing completed (~1s) BEFORE
  service discovery (~2s), so `handle_auth_complete`'s resume hook never fired — the
  normal bonded branch (bond present by the time discovery finished) carried it to
  READY. The hook only runs if the pump offers `SEC_REQ` AFTER discovery completes
  (and within the ~430ms post-discovery window), which this pump doesn't seem to do
  (it offers at connection establishment). Keep it as correct insurance for a
  late-offering pump, but note it is UNVALIDATED by live capture — not a bug, just
  untested. If ever revisited: find a way to exercise the discovery-first ordering,
  or accept it as defensive code.

- [ ] **Race #2 — operation-ordering race (subscribe vs. encryption). INCORRECT
  CODE, mitigated but not fixed.** On the bonded path (Chunk 2 Option A), the CCCD
  subscribe write is *issued* concurrently with the encryption request, so it can
  reach the pump BEFORE encryption completes. The pump sometimes rejects that
  ordering with a `0x13` disconnect; it's nondeterministic (next attempt usually
  succeeds). Confirmed live 2026-06-26 in `chunk5-2.log` (19:31:49): `0x13` ~90ms
  after `Auth mode: 0x09`, self-recovered on the next open. See DESIGN_NOTES.md
  Chunk 2.5 (race #2 diagnosis) and the Option B sections.
  - *Severity:* the readiness gate (Chunk 2) made the *consequences* mild — the
    BOND is never lost, only the connection bumps and retries. But relying on a
    race is still incorrect; correct BLE practice is encrypt-before-subscribe.
  - *Candidate fix:* Option B (gate subscribe behind encryption-complete). BUT a
    prior Option B attempt (Claude-on-web) had problems and was rolled back —
    DO NOT just re-apply it. Any revisit needs a thorough fresh analysis of why
    that attempt failed (and the ~430ms encryption-start deadline interaction).
  - *Priority:* TBD — correctness debt, not blocking; pump power-cycles are rare.

- [ ] **Component-wide assumption: no BLE MAC rotation.** alpha-hwr (v0.4.0,
  unfinished) connects to a fixed MAC from a YAML substitution, so RPA/address
  rotation is assumed away throughout. `peer_bond_exists()`'s address-match relies
  on this. We stay this way until proven otherwise — but if a future pump rotates
  addresses, the whole connect-by-fixed-MAC foundation (not just this function)
  needs revisiting. Documented dependency, not an action item yet.

- [ ] **Transient bond-read failure (minor, watch-only).** If
  `esp_ble_get_bond_device_list()` returns non-`ESP_OK` on a genuinely-bonded
  reconnect, `peer_bond_exists()` yields a one-cycle false "NO BOND" → quiet-listen
  → self-corrects on the next reconnect's re-read. Low impact; no change planned.

---

## Notes

- Larger deferred work (pyscript `verify_pump_config` retry, telemetry scaling) is
  tracked in `DESIGN_NOTES.md §9`.
</content>

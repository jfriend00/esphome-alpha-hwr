# Upstream Contribution Notes

A running record of changes made in this fork that are candidates for contributing
back upstream, written **for the maintainer**. Each entry is framed as
**problem → solution (why and how)**, followed by the testing done and the open
questions a reviewer would want to weigh.

These notes are intentionally free of this fork's internal development shorthand;
they describe behavior and rationale a reviewer needs, nothing about how the work
was sequenced.

**Scope:** only component-level changes (in `components/alpha_hwr/`) are tracked here.
Device-specific configuration and diagnostic controls in the example device YAML
(e.g. the bond-clear and force-disconnect buttons) are local to this setup and are
not part of an upstream contribution.

**Tested hardware (single unit):** Seeed XIAO ESP32-S3 + W5500 Ethernet → Grundfos
ALPHA HWR pump, ESPHome 2026.6.2, no WiFi/proxy. All BLE error and disconnect codes
below are as reported by ESP-IDF Bluedroid.

---

## 1. Preserve the bond: request encryption only after service discovery

### Problem

Requesting encryption immediately on connection-open can destroy the stored bond and
force a manual re-pair. A pump that has just powered up advertises and accepts a GATT
connection *before* its BLE stack is ready to complete a security handshake. If the
central requests encryption in that window, the attempt fails with an encryption-start
failure (0x61), and on that failure the ESP-IDF stack clears the device's stored bond
keys. Subsequent reconnects then find no bond, attempt pairing, get rejected, and loop
forever until a manual re-pair. The failure is a race: the same power-cycle sometimes
reconnects cleanly and sometimes loses the bond, depending on timing.

### Solution (how)

Use successful, unencrypted **service discovery as the readiness signal**. The
encryption request is moved out of the connection-open handler and issued only after
service discovery completes — discovery succeeding proves the pump's stack is ready, so
the encryption attempt lands when the pump can satisfy it. Encryption is requested from
exactly one place, so the ordering is guaranteed by construction rather than by timing.

### Result / testing

Across repeated cold pump power-cycles, the bond is preserved and the device reconnects
and re-encrypts cleanly; the bond-clearing failure signature (0x61) no longer appears.

### Why this approach, and not the alternatives

Preventing the failed attempt is the only practical fix short of patching the
framework:

- The key clear on encryption failure is **unconditional** in ESP-IDF Bluedroid (it
  fires on KEY_MISSING / AUTH_FAILURE / ENCRY_MODE_NOT_ACCEPTABLE, with no config or
  sdkconfig option to disable it), so it cannot simply be turned off.
- There is **no public API to write a bond back** (`esp_ble_get_bond_device_*` can read
  bonds, but nothing restores one), so a snapshot/restore backstop is not possible.
- Reloading from NVS does not help either — the failed attempt clears the *persisted*
  key, not just the runtime record.

So the practical lever is timing: don't request encryption until the pump is
demonstrably ready.

*(This change is the foundation the next entry builds on.)*

---

## 2. Reliable (re-)pairing: wait for pump-initiated pairing when unbonded

### Problem

Establishing a *new* bond was unreliable and could take many minutes of retries:

- With no bond, the device requested encryption as soon as it was ready — a
  central-initiated SMP pairing request. The pump rejects that with "Pairing Not
  Supported" (0x52) unless it is already in pairing mode.
- Each 0x52 rejection drops the link and triggers a ~2.5s reconnect. The device is
  therefore frequently mid-reconnect (disconnected) at the exact moment the pump
  finally offers to pair — so it misses the window.
- Every captured pairing *success* had the same shape: a run of 0x52 rejections while
  the central asked and the pump wasn't offering, then — the instant the **pump** sent
  its own security request (SEC_REQ) — pairing completed in ~1 second.

Conclusion: for this pump, a new bond is **pump-initiated**. The pump's SEC_REQ is both
the "I am in pairing mode now" signal and the trigger that makes pairing succeed. The
central's eager request only produces rejection noise and reconnect churn that competes
with the pump's offer.

### Solution (how)

Once service discovery completes, branch on whether a bond already exists:

- **Bond present** (normal reconnect): request encryption and subscribe, as before.
  Encryption against an existing bond is central-initiated and works normally.
- **No bond** (initial pairing / re-pair): stay quiet. Issue no central request, and
  defer the notification subscription as well — an unencrypted CCCD write before the
  link is encrypted is rejected with a 0x13 disconnect. Accept the pump's SEC_REQ
  whenever it arrives (already handled), and once pairing completes, run the deferred
  subscribe over the now-encrypted link.

A single flag coordinates the deferred subscribe. The bonded-reconnect and
passive-telemetry paths are unchanged.

This relies on a property of the ESPHome BLE client base: it never initiates encryption
on its own — the component is the sole trigger. So suppressing the component's request
genuinely leaves the link quiet for the pump to drive, with no base change required.

### Result / testing

- **Re-pair:** after a period of completely silent quiet-listen cycling (zero 0x52
  emitted by the device), the moment the pump was placed in pairing mode it sent
  SEC_REQ and pairing completed to a working, subscribed state in ~1 second. The device
  produced no 0x52 noise at any point.
- **Normal bonded reconnect** (including pump power-cycle): unaffected — reconnects and
  re-encrypts as before, bond preserved.
- The device is now deterministically receptive; the remaining time-to-pair is
  dominated by getting the pump itself into pairing mode (its own button UX), not by the
  device missing the offer.

### Open question for the maintainer (anticipated pushback)

This makes the no-bond path **pump-initiated only** — the central-initiated pairing
request is no longer issued when unbonded. It is correct for the single unit tested,
and one would expect the ALPHA HWR family to share pairing firmware, but that is
unverified across other units, firmware revisions, or regional variants. If any variant
expects the *central* to initiate pairing (the BLE default), it would not pair under
this change; it would sit waiting for a SEC_REQ that never comes.

We deliberately did **not** add a configuration switch for the initiator strategy: we
can only test one unit and did not want to ship a knob whose alternative path we cannot
exercise. If broader testing shows mixed behavior across units, the clean fix would be
an option such as `pairing_initiator: pump | central`. This is flagged here so the
assumption is explicit rather than hidden, and so the maintainer — who may have access
to more units — can decide whether to gate it, change the default, or accept it as-is.

One caveat for any future hybrid approach: a timeout-based "wait for the pump, then fall
back to central-initiated" strategy is **not** a safe default on this pump. Firing the
central request reintroduces the 0x52 rejection and the reconnect churn that this change
removes, so a fallback timer would resurrect the original problem unless tuned very
conservatively. A simple explicit choice is safer than an adaptive hybrid.

---

## 3. Known limitation: subscribe/encryption ordering on bonded reconnect

### What it is

On the bonded-reconnect path, the encryption request and the notification subscription
(the CCCD write) are issued concurrently once service discovery completes; the code does
not enforce "encrypt before subscribe." Usually encryption wins and the sequence
completes normally, but occasionally the CCCD write reaches the pump first, the pump
rejects the unencrypted write, and the link drops (0x13). The device then reconnects and
almost always succeeds on the next attempt.

### Impact

Benign in practice: the bond is never lost (this is a connection-ordering issue, not a
key failure), and the device self-recovers on the following reconnect — at worst a few
seconds of extra churn after a pump power-cycle. It is, however, a correctness gap: the
ordering is won by timing rather than guaranteed.

### Why it isn't fixed here

The clean fix is to serialize the subscribe behind encryption-complete (issue the CCCD
write only after the authentication-complete event). That was prototyped and reverted
after it caused problems under test, so it needs a fresh, careful implementation rather
than a quick re-apply. Recorded here so a reviewer is aware of the concurrent ordering
and the intended direction.

---

## 4. Diagnostic logging included

These changes add a small amount of INFO-level diagnostic logging that ships with the
component: a `peer_bond_exists()` helper (which queries the ESP-IDF bond store) plus log
lines reporting the bond state at connection-open and after an authentication failure.
They were invaluable during diagnosis and are cheap, but a maintainer may prefer to drop
them to DEBUG level.

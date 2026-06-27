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

Across multiple cold pump power-cycles, the bond is preserved and the device reconnects
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
- **No bond** (initial pairing / re-pair): stay quiet. Issue no central request,
  and defer the notification subscription as well — an unencrypted CCCD write
  before the link is encrypted is rejected with a 0x13 disconnect. Accept the
  pump's SEC_REQ whenever it arrives (already handled), and once pairing
  completes, run the deferred subscribe over the now-encrypted link.  The device
  does not hold a single connection open waiting indefinitely; the pump tears
  down an idle unencrypted connection on its own, so the quiet-listen path
  naturally cycles through reconnects until the pump's SEC_REQ arrives. No
  central-side wait timeout is needed, as the pump drives the retry cadence.

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

## 3. Self-healing reconnect: cancel in-flight handshake work on disconnect

### Problem

After an unexpected disconnect — a pump power-cycle, or any drop mid-handshake — the
device could fail to recover and instead cycle through reconnect attempts without
reaching a working state. Disconnect did not tear down the work that was in flight, so
tasks from the previous connection executed against the next one.

The disconnect handler only transitioned the session back to IDLE and reset the
transport. It did not cancel the authentication handshake, did not stop the telemetry
service (leaving its running flag set), and did not cancel the pending post-subscribe
"stabilize, then authenticate" timer. Three pieces of stale state therefore survived
into the next connection:

- **The staged authentication handshake kept running.** Its scheduled stage callbacks —
  and its completion — fired against the new, not-yet-ready connection: auth packets were
  sent before service discovery had resolved the characteristic handles (so the writes
  failed), and authentication "completed" while the new session was still in service
  discovery.
- **The ~2-second stabilize/authenticate timer was an anonymous one-shot** with no
  handle, so it could not be cancelled. A disconnect during that window left it to fire
  in the next cycle and kick off a second, mistimed authentication.
- **The telemetry service's running flag was never cleared,** so the next connection saw
  the service as already running and proceeded from inconsistent state.

The visible signature was a run of out-of-order transitions — authentication completing
"from unexpected state: SERVICE_DISCOVERY," service discovery arriving "from unexpected
state: AUTHENTICATING," and characteristic writes failing before the handles were
resolved — after which the pump terminated the malformed connection (0x13) and the cycle
repeated. Because each reconnect inherited the corrupted state, the device could not
reliably self-heal.

### Solution (how)

Make disconnect a clean teardown so every connection begins from a known state. In the
disconnect path, in addition to the existing session-to-IDLE transition and transport
reset:

- Cancel the in-flight authentication handshake.
- Stop the telemetry service, clearing its running flag.
- Cancel the pending stabilize/authenticate timer.

Two of these already existed in the component — the authentication object's cancel
routine and the telemetry service's stop routine — and were simply never invoked from the
disconnect callback; the fix wires them in rather than adding new machinery. The stabilize
timer was made cancellable by giving it a name (it was previously anonymous). The staged
authentication callbacks are already guarded by an internal sequence counter, and
cancelling authentication advances that counter, so any still-pending stage callbacks
no-op on their own — the stabilize timer was the only scheduled item that needed an
explicit cancel. All three teardown calls are no-ops when nothing is in flight, so they
are safe to run on every disconnect.

### Result / testing

The failure was captured directly. In a reconnect cycle — notably with pairing
*disabled* (passive telemetry mode), which takes SMP entirely out of the picture and
rules out the bond/pairing machinery as the cause — the stale-work signature appeared
exactly as described: authentication "completed" while the session was still in
SERVICE_DISCOVERY ("on_authenticated() called from unexpected state: SERVICE_DISCOVERY")
and the telemetry service reported "Service already running"; then the leftover stabilize
timer fired and started authentication *before* service discovery completed and before
notifications were subscribed, so the auth stage tried to write before the GENI
characteristic handle existed — five consecutive "Failed to send chunk" errors — and the
connection collapsed back into the loop. That this reproduced with pairing off is the key
point: it is a teardown/reentrancy defect, independent of the BLE security work in
entries 1–2.

After the fix (in the current build), subsequent reconnect captures — including a pump
power-cycle — complete in the intended order (discover → subscribe → wait → authenticate)
and reach a working state, with none of the stale-task lines above. A dedicated
side-by-side capture of a mid-handshake disconnect was not preserved, so this rests on
the captured failure plus the absence of the signature in later runs rather than a single
before/after pair.

### Why this approach, and not the alternatives

- Cancelling the in-flight work at the point of disconnect is the direct fix: the defect
  is precisely that tasks outlived the connection they belonged to. Reusing the
  component's existing cancel and stop routines keeps the change small and low-risk.
- Guarding the authentication entry point to bail out when the link is no longer connected
  would mask the symptom but leave the stale timer scheduled and the telemetry flag set;
  cancelling at the source also avoids the wasted reconnect churn.
- Naming the stabilize timer is the minimal change that makes it cancellable, and it
  mirrors the sequence-counter guard the authentication callbacks already use.

---

## 4. Known limitation: subscribe/encryption ordering on bonded reconnect

### What it is

On the bonded-reconnect path, the encryption request and the notification
subscription (the CCCD write) are issued concurrently once service discovery
completes; the code does not enforce "encrypt before subscribe." This is how it
works in the baseline code and not something that occurs because of these changes.
Usually encryption wins and the sequence completes normally, but occasionally
the CCCD write reaches the pump first, the pump rejects the unencrypted write,
and the link drops (0x13). The device then reconnects and almost always succeeds
on the next attempt.

### Impact

Benign in practice: the bond is never lost (this is a connection-ordering issue, not a
key failure), and the device self-recovers on the following reconnect — at worst a few
seconds of extra churn after a pump power-cycle. It is, however, a correctness gap: the
ordering is won by timing rather than guaranteed.

### Why it isn't fixed here

The clean fix is to serialize the subscribe behind encryption-complete (issue
the CCCD write only after the authentication-complete event). That was
prototyped and reverted after it caused problems under test and since it didn't
cause a last problem, we moved onto more pressing matters, so it needs a fresh,
careful implementation rather than a quick re-apply. Recorded here so a reviewer
is aware of the pre-existing concurrent ordering and the intended direction.

---

## 5. Diagnostic logging included

These changes add a small amount of INFO-level diagnostic logging that ships
with the component: a `peer_bond_exists()` helper (which queries the ESP-IDF
bond store) plus log lines reporting the bond state at connection-open and after
an authentication failure. They were invaluable during pairing issue diagnosis
to see exactly when a bond gets lost and what happens afterwards and are cheap,
but a maintainer may prefer to drop them to DEBUG level.

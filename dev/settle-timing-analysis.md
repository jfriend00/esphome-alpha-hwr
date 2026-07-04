# ALPHA HWR reconnect_settle_time — floor-test timing analysis

Device: XIAO ESP32-S3 (dual-core Xtensa, 240 MHz). Converge tree. Floor testing 2026-07-02.
Pump: Grundfos ALPHA HWR, BLE addr 0C:4B:EE:11:B6:C9.
Source logs (same dir): converge-{50ms,100ms,250ms,500ms}.log ; ble_advertise.log.

## The problem being sized

On a BONDED reconnect, the ESP32 requests encryption on connection open. If the pump
is not yet ready to resume the bonded (encrypted) session, the request goes unanswered,
ESP-IDF times out (~6 s) and reports 0x61 (Encryption Start Failed), and the bond is
erased. Recovery is a physical re-pair at the pump. reconnect_settle_time holds off the
reconnect after the pump reappears, giving the pump time to become ready before encryption
is requested.

## 1. Pump advertising interval (from ble_advertise.log)

35 advertisements over a 5 s window.
- min gap 75 ms, mode ~100 ms (cluster 75-124 ms).
- larger gaps (200/300/591 ms) are advertisements our scan missed = 2x/3x/6x the base.
- Conclusion: pump advertises ~every 100 ms (~10 Hz). Fast and reliable.
- Advertisement spacing is NOT the timing risk.

## 2. The pump readiness window W (reappear-to-encryption bracket)

Timed from "Pump reappeared" (first advertisement seen after disconnect) to the encryption
request (the too-early action when it fails).

| settle | reappear-to-encryption | outcome                |
|-------:|-----------------------:|------------------------|
|  50 ms | 316 ms                 | FAIL (0x61, bond erased)|
| 100 ms | 731 ms                 | ok                     |
| 100 ms | 948 ms                 | ok                     |
| 250 ms | 763 ms                 | ok                     |
| 250 ms | 827 ms                 | ok                     |
| 500 ms | 846 ms                 | ok                     |
| 500 ms | 956 ms                 | ok                     |

- FAIL at 316 ms => W > ~316 ms (solid lower bound).
- min success 731 ms => W <= ~731 ms (soft upper bound: assumes we caught the pump's
  first advertisement; if we missed early ones, true W is higher).
- **Window bracket: ~0.32 s < W < ~0.73 s.**

## 3. Why the floor looked misleadingly low: tail decomposition

The "tail" (settle-elapsed to encryption) splits into advertisement-catch + link-setup:

| run          | adv-catch | link-setup | tail   | total (reappear-to-enc) | outcome |
|--------------|----------:|-----------:|-------:|------------------------:|---------|
| 50 ms (FAIL) | 49 ms     | 200 ms     | 263 ms | 316 ms                  | FAIL    |
| 100 ms       | 111 ms    | 502 ms     | 628 ms | 731 ms                  | ok      |
| 100 ms       | 111 ms    | 719 ms     | 845 ms | 948 ms                  | ok      |
| 500 ms       | 110 ms    | 215 ms     | 343 ms | 846 ms                  | ok      |
| 500 ms       | 110 ms    | 328 ms     | 452 ms | 956 ms                  | ok      |

- adv-catch is tight (~50-110 ms = one advertising interval). Not the variable.
- link-setup is the noisy term: 200-719 ms, 3x variable. THIS dominates.
- The total barely tracks the settle value (100/250/500 ms all landed 731-956 ms) because
  link-setup swamps the settle step.

## 4. Root cause of link-setup variance: 0x3e retries (PUMP-driven)

Long setups contain a failed first connection attempt:

    Connecting                             (connection request sent)
    BT_HCI: hcif disc complete ... rsn 0x3e  (attempt FAILED, ~270 ms)  <-- 0x3e =
    Connection open                        (retry succeeded, ~450 ms)     "Connection
                                                                           Failed to be
                                                                           Established"

- Fast setups (~200 ms): connection request accepted first try (no 0x3e).
- Slow setups (500-720 ms): first attempt fails 0x3e (pump does not complete the LL
  handshake), controller times out ~270 ms, retry succeeds ~450 ms later.
- Primary cause: PUMP responsiveness. A freshly-advertising pump advertises promptly but
  fumbles the actual connection. Same not-ready theme as the encryption window.
- NOT the ESP32 app CPU (this is radio-controller level).
- RF packet loss is a secondary contributor (also surfaces as 0x3e).

## 5. Key conclusion: clean fast connections are the DANGEROUS ones

The link-setup "padding" that saves low-settle runs is the pump FAILING to connect (0x3e)
and buying 300-500 ms of accidental delay. When the pump instead connects cleanly on the
first try (~200 ms), encryption is requested fast, while the pump still is not ready => bond
loss. The 50 ms failure was a clean first-try connection; both 100 ms survivors were 0x3e
fumbles. You cannot rely on the fumble to protect you.

Therefore the settle must cover the window on its own:
- free coverage on a clean/fast draw = adv-catch (~50 ms) + link-setup (~200 ms) = ~250 ms
- window upper bound = ~730 ms
- **real reliability floor = 730 - 250 = ~480 ms** of settle, before any margin.

100 ms "passed" only because both draws were 0x3e fumbles. A clean-connection draw at
100 ms totals ~350 ms and lands inside W = bond loss. So more 100 ms runs WOULD eventually
fail, specifically the ones that connect cleanly.

## 6. Default recommendation: 2 s

- ~4x the ~480 ms real floor; ~3x the ~730 ms measured window.
- swamps the entire observed link-setup range (200-720 ms).
- headroom for the soft upper bound on W, cold-boot variability, and faster future silicon
  (shorter setups mean the settle carries more of W).
- cost is a couple of seconds on reconnect, invisible for a recirc pump; failure is a
  physical re-pair. Asymmetric downside favors generous margin.
- Defensible band 2-3 s. Do not go below ~1.5 s. No benefit above ~3 s.

## Caveats
- Small sample: 1 fail cycle, 6 success cycles, one device (S3).
- Upper bound on W is soft (advertisement-miss anchoring).
- Cold-boot readiness after a long power-off (e.g. water-heater maintenance) not separately
  characterized; quick power-cycles only.

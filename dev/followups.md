# Follow-ups to revisit

Personal tracker for interim / incomplete fixes and open questions to check back on once eman appears finished with the related work.

eman's pattern is often to first attenuate a problem's impact with a safe but not fully complete fix, then return to a more complete fix later. These notes flag where a first-pass fix left something on the table, so I can verify the complete fix eventually landed, or raise it myself if it stalled.

## Still open (as of 2026-07-08)

Quick-glance list; details in the sections further down and in memory.

1. **Class 3 START/STOP for pump enable — WATCH ONLY.** Not switched yet; the #52 500ms readback groundwork is in, the command change isn't. When it lands it retires the last setpoint issue (the #43 startup-cache-window: enabling before the first setpoint read still sends the 3671 default). No action needed, just watch for it.

2. **Remote Mode — functional behavior still unknown.** Readability is RESOLVED (the Sub 7 fix, hardware-verified: control_source reads 2 when remote active). Still open: whether *engaging* remote actually changes pump behavior, other controls, or the internal schedule. Live because remote auto-fires on every pump enable (YAML lambda in the Pump Enabled `turn_on`) and flips a real control-priority state. Tracked by: my #61 comment asking eman if he's seen it affect behavior (awaiting reply); #63, the end-to-end harness proposal, which would settle it empirically; and my fork fallback of commenting out the `enable_remote()` line on adoption. Minor loose thread: eman's Sub 7 test showed remote=2 but not local=1 or clearing on a panel override.

3. **Adopting eman's work into my fork — the big pending task.** Still running the instrumented fork on the old opcode; haven't taken any of today's changes. Deliberately WAITING until the rapid-change period settles (I have a working system now; will adopt at a convenient stable point, a tagged release is the clean endpoint, could be 0.8.0 / a pending 0.8.5 / 0.9.0 / main). When adopting: pull the base..tag diff scoped to `components/alpha_hwr/` + `packages/`, organize by provenance, review with the lifecycle-risk lens (does anything touch the bond-loss/auth/reconnect code from #9/#11/#15/#17/#19/#22) and skepticism of the AI-generated churn (some was wrong and reverted). Folded in: the `enable_remote()` comment-out decision (item 2); verify the #54 disable-polling bug is fixed (`control_state_poll_interval: 0` failed validation per Copilot; want polling OFF as exclusive-control); scrutinize the #28 centralize-GENI-frame refactor (byte-identical-on-the-wire check) and #32 memory-leak fix (churn not tracked closely).

**Resolved:** flow echo-vs-real-value (eman vindicated, no readable register exists), per-mode caches (#51/#57), #43/#44 coupling, transport ACK race (#50 / commit 31a57b9), #43/#44/#45/#46 proper, #61 remote-state readability (Sub 7).

---

## #44 Constant Flow Setpoint display: interim echo fix, complete fix still open

**Related:** issue #44, PR #48 (diagnostic), issue #43 (coupled, see below).

**Status (2026-07-07):** eman confirmed the root cause on hardware and proposed an interim fix.

**Root cause (confirmed on the live pump):** In Constant Flow mode, Class 10 Object 86 / Sub 6 does not carry a usable flow setpoint. It returns a fixed value (0.000694 m³/h, which converts to 0.003056 gal/min, exactly the bad value I originally reported) regardless of what setpoint was commanded. The same register reads the setpoint correctly for Constant Speed and Constant Pressure, so this is specific to flow mode. The register is simply the wrong source of truth for the flow setpoint display.

**eman's proposed fix (interim):** For flow mode, display the last client-commanded value (cached optimistically in `cached_setpoint_` after `set_constant_flow_async()` writes it) and ignore the register readback, similar to how the pressure modes are special-cased.

**Why I think this is incomplete:** It displays what the client last *commanded*, not what the pump actually *holds*. This is the same pattern that made the Remote Mode switch untrustworthy: reporting the local intent instead of the real pump state. For a setpoint that can drift, the pump clamping it to a min/max, an external change, or a silent write failure, the display would confidently show the wrong value. It covers up the symptom rather than reading the true value.

**The complete fix I think is right:** Find where the pump actually reports its stored flow setpoint in telemetry and display that. This is the pump's *target* flow setpoint, which is distinct from the Flow Rate sensor (the *measured / achieved* flow, which already works fine).

**Why the complete fix is likely achievable, not just a principle:**
- The pump obviously has a flow setpoint. It is actively regulating to it.
- Object 86 / Sub 6 reads the setpoint correctly for Constant Speed and Constant Pressure, so this pump can and does report setpoints. It is just that this one register's setpoint field is not the right source in flow mode.
- So the real flow setpoint is almost certainly in a different register / object, which should be documented in eman's `resources/` reverse-engineering corpus (the GENI profiles / register map). He has not checked there yet. He tested one register, found it garbage, and moved to the echo.

**Fair caveat (the case for eman's fix):** For a setpoint specifically (unlike a live sensor), an optimistic echo of the commanded value is a reasonably accepted Home Assistant pattern. Many setpoint entities are optimistic because devices do not echo them back. So the echo is a defensible *fallback*, but only if the pump genuinely does not expose the setpoint, which there is good reason to doubt.

**Coupling with #43 (watch this too):** eman's #44 register-suppress is also what makes #43's flow-mode setpoint reuse correct. The register overwrites `cached_setpoint_` with 0.000694 for flow, and #43's `start()` reuses `cached_setpoint_` (flow is in its allow-list). So with #43 landed but #44 not, enabling the pump in flow mode would reuse 0.000694 and command essentially zero flow. #43's "flow already resolved" claim only holds once #44's register-suppress lands. Confirm they land together, or #44 first.

**To check later (once eman appears done):**
1. Did he follow up with a telemetry-sourced fix that reads the pump's actual flow setpoint from the correct register, or did the echo become the permanent solution?
2. If the echo is permanent: is it because the pump genuinely does not expose the flow setpoint anywhere readable (confirmed from the profiles), or because it was not investigated? If the latter, raise the register question and/or propose the read-based fix.
3. Confirm #44 and #43 landed together (or #44 first) so flow-enable was not broken in the interim.

**Framing if I raise it (a question, not "your fix is wrong"):** "Before falling back to echoing the commanded value, is the flow setpoint readable from a different register than Object 86 / Sub 6? It reads fine for speed and pressure and the pump is clearly regulating to a flow target, so the true value is probably in the profiles. Displaying that would avoid the same 'shows local, not pump' weakness we hit with Remote Mode."

**Update (2026-07-08):** PR #48 now includes the actual register-suppress fix (not just the diagnostic). Intended behavior: flow setpoint shows "unavailable" until the user explicitly sets one, then echoes the commanded value, so it fails safe rather than showing a wrong number (more defensible than a naive echo). BUT GitHub Copilot's automated review independently found the workaround already leaking: `cached_setpoint_` is a single variable shared across all modes, and the fix leaves it untouched in flow mode assuming it's NAN. It is NOT necessarily NAN, if the pump enters flow mode via a notification (pump-initiated switch, not a user `set_mode()` which clears it), `cached_setpoint_` retains the leftover value from the previous mode (e.g. a stray 2000 RPM from speed, or a pressure-in-meters value), so instead of "unavailable" the flow display shows that stale value. Copilot's suggested patch (reset cache to NAN on entering flow) is correct and is what makes eman's intended fail-safe actually hold. TAKEAWAY: this is (a) a real bug in the interim fix, and (b) independent corroboration of the architectural point above, an outside reviewer found the cache/echo workaround has cross-mode edge cases a true readback wouldn't. Copilot also flagged a stale PR description (changelog claims bench-verified 2026-07-08, PR description still says "hardware verification needed"); eman did bench-test, so the PR description just needs updating.

**Better design than Copilot's reset patch (my idea, 2026-07-08): per-mode cached setpoints.** Copilot's "reset the shared cache on entering flow" patches one leak path but keeps the fragile single shared `cached_setpoint_`, so a future notification route / mode transition can reintroduce the same contamination. A SEPARATE cached setpoint per mode (speed / pressure / flow / etc.) makes cross-mode contamination structurally impossible (no shared slot for a stale value), needs no reset logic, and lines up one-to-one with the per-mode setpoint entities the component already exposes (bonus: each mode remembers its own last setpoint across switches). SCOPE NOTE: per-mode caches fix the CONTAMINATION only; orthogonal to the echo-vs-real-value concern (flow's cache is still the client echo until the real flow-setpoint register is found). Fully-robust design = per-mode caches (structure) + each cache sourced from the pump's actual value where the pump exposes it. TO CHECK LATER: does eman take the minimal reset patch, or the cleaner per-mode refactor? Consider floating per-mode as a design suggestion ("makes contamination structurally impossible + matches your per-mode entities"), his call on refactor-vs-patch.

**Update (2026-07-08): now THREE cross-mode-contamination instances on the shared `cached_setpoint_`**, each patched individually: (1) `set_mode()` already clears it; (2) #44 flow path leaks a stale value (Copilot's reset-on-entry, pending); (3) #47/#43 `start(mode)` left a stale value a later `start()` resent under the wrong mode's units (eman fixed in 24c80b3 by mirroring set_mode's clear; example: 4.0m pressure resent as bogus 4.0 RPM speed; +test). So eman is now visibly playing whack-a-mole with the shared variable — strong, concrete validation of the per-mode-cache proposal (would eliminate the whole class, zero clears needed). GOOD MOMENT to float per-mode to eman with the three instances as evidence ("you've added cross-mode clears in set_mode, start(mode), and the flow path; a per-mode cache makes all three unnecessary + un-regressable + matches your per-mode entities") — reads as design, not criticism, since it's backed by his own bug trail.

**RESOLVED into issue #51 (2026-07-08).** Floated per-mode caches as a review comment on PR #47; eman fully agreed and filed **issue #51** ("Refactor `ControlService::cached_setpoint_` to per-mode storage"), crediting the origin comment. He confirmed all three contamination sites, cited the existing `cached_temp_min_`/`cached_temp_max_` per-field precedent, and enumerated the full surface area himself: the field, the four setpoint setters, both mode-update paths (`update_mode_from_notification()`, `get_mode_async()`'s callback), `start()`'s setpoint-reuse logic, `get_cached_setpoint()`, and the four YAML number entities in `packages/alpha_hwr_controls.yaml`. Parked by design until #47/#48/#49/#50 merge and release, to avoid conflicting with the in-flight PRs.

Two things to verify when #51 is picked up:

1. **The refactor must DELETE the three now-redundant clear sites, not just add per-mode fields alongside them.** The whole point is to retire the "remember to invalidate on every transition" pattern, so the clears in `set_mode()`, in `start()` (added in #47), and in #48's flow path should come out. Once each mode has its own field, those clears guard nothing: a per-mode field can only ever hold its own mode's value. If the fields go in but the clears stay, the change is net *more* code and has not actually killed the fragility. #51's scope bullets list the additions but do not explicitly call out removing the clears, so this is the success criterion to watch.

2. **#51 is contamination-scoped only. It does not resolve the flow echo-vs-real-value concern above.** Per-mode storage gives Constant Flow its own cache slot, but that slot is still fed by the client-commanded echo (the Object 86 / Sub 6 readback is the fixed 0.000694 garbage and is suppressed). So a later "setpoint caching is clean now" status must not be read as also closing the "read the pump's actual flow setpoint" question. Keep the two distinct.

Bonus of per-mode fields: each mode naturally remembers its own last setpoint across mode switches. Open implementation choice (eman's call): named per-mode fields (matches the temp precedent) versus a small mode-indexed lookup (a slightly DRYer accessor).

---

## Status update (2026-07-08) — after the v0.8.0 + #55–#60 batch

- **Per-mode caches (#51): SHIPPED and correct — DONE.** Merged as PR #57. `start()` now reads the per-mode fields (`cached_pressure_setpoint_` / `cached_speed_setpoint_` / `cached_flow_setpoint_` / `cached_proportional_setpoint_`) and the three redundant clears were removed (the success criterion). Cross-mode contamination is now structurally impossible, not just guarded. This thread is closed.

- **#43/#44 coupling: RESOLVED.** With register-suppress (#44) and per-mode caches (#51) both shipped, `cached_flow_setpoint_` holds the client-commanded flow value, not the `0.000694` garbage, so `start()` reusing it no longer commands ~zero flow. No longer a concern.

- **Flow setpoint echo-vs-real-value: STILL OPEN — the sole remaining item in this file.** Nothing in today's batch touched it. Flow's per-mode cache is still fed by the client echo (Object 86/Sub 6 stays suppressed as garbage). The complete fix — read the pump's *actual* stored flow setpoint from the correct register — has never been attempted. Corpus investigation for the real register is now in progress (findings to be appended).

- **Class 3 START/STOP for pump enable: NOT switched yet (as of 2026-07-08).** `start()`/`stop()` still use Class 10 `send_control_request()` with the #43 cached reuse (verified: PR #55 only *added* the 500ms `get_mode` readback, it did not change the command type; no merged PR flips enable to Class 3). eman's #52 issue + PR #55 discuss "Class 3 START/STOP" and pre-fix the UI lag those would cause — read as **prerequisite groundwork** for a future switch, held for a not-immediate release (nothing about Class 3 enable appeared before 0.8.0). WATCH FOR IT: when enable moves to Class 3 START (pump uses its own stored setpoint, no client setpoint sent), it eliminates the #43 **startup-cache-window caveat** (enable before the first setpoint read still sends the 3671 default). Until then, that caveat remains.

### Flow-readback investigation RESULT (2026-07-08) — echo is CORRECT, not a cover-up

A full search of both eman repos (Python `alpha-hwr` + C++ `esphome-alpha-hwr`) found **no readable flow-setpoint register on the ALPHA**:
- The standard GENIbus reference registers (`ref_act`, `ref_rem`, `ref_norm`, `q_ref`, etc.) do **not exist** anywhere in the corpus (zero hits) — not mapped for this pump.
- Read path = the known-garbage Class 10 Object 86/Sub 6 (fine for speed/pressure, fixed `0.000694` for flow).
- Write path (`set_constant_flow`) = Object 86/**Sub 39**, but `SUB_FLOW_SETPOINT == SUB_FLOW_LIMIT == 39` (same sub-ID), so 86/39 is the **max-flow-LIMIT** object, not a target, and is never read back.
- Only "setpoint" telemetry object defined is RPM (speed), reserved/unused; measured flow is a separate object and already works.

**Fairness correction:** the earlier "echo = the Remote-Mode cover-up anti-pattern" critique (see the "Why I think this is incomplete" section above) does **not** hold. Remote Mode's `control_source` was readable and discarded (avoidable); the flow *target* is genuinely not readable, so the client echo is the only option and a correct one. eman was right on #44; nothing to raise with him.

**Likely mechanism:** the `SUB_FLOW_SETPOINT == SUB_FLOW_LIMIT` collision suggests the ALPHA implements constant-flow as a flow-*limited* mode, not a true closed-loop target — which explains the absence of a readable flow setpoint.

**One residual (theoretical):** definitive source is the external `geni_profile_52_7.xml` Grundfos profile, referenced in eman's `constants.py:193` (consumed by `profile_parser.py`, register addr = `(geni_class << 8) | geni_id`) but NOT committed to either repo. If it ever surfaces and maps a flow-reference object, revisit; since eman references it himself and still didn't map one, it's very likely not there.

**Disposition: RESOLVED.** Echo is correct given no readable register. Local copies of the cited corpus files are in the session scratchpad (`ahwr/`: `services/control.py`, `constants.py`, `control_service.cpp`, `control_service.h`, `data_models.md`).

# Git Workflow: keeping contribution and personal work separable

This fork carries two kinds of change that live in the **same files**:

- **Contribution-worthy** — bug fixes / features in `components/` that should go upstream
  to `eman/esphome-alpha-hwr`.
- **Personal** — device config (`device/`), packages tweaks for this setup, personal
  features (e.g. the speed-control shortcut), notes.

Because they share files, you can't separate them by *file* alone. Separation comes from
**how the work is structured** (§1) plus **commit discipline** (§2–§6).

**The hard constraint that shapes everything:** there is one device, it runs a live
multi-tier system in the house, and there is no second unit and no automated test harness.
So a contribution-only branch can't be built or run in isolation. That means the goal is:

> **Make each extracted contribution byte-identical to the component code the house is
> already running — so production *is* the test.**

Everything below serves that goal.

Branches in play:
- `main` — a **pristine mirror of `upstream/main`**. Never commit to it directly.
- `bt_issues` — your one long-lived working/dev branch (what the device builds). Everything
  you do lands here.
- `contrib/<topic>` — short-lived, clean branches off `upstream/main`, created only when
  you're ready to open/refresh a PR. The PR source. Never your daily workspace.

---

## 1. The core strategy: keep the two tracks from overlapping *in code*

"Production is the test" only holds if a personal change never edits the same code a
contribution edits. If they overlap, the extracted contribution is a combination that has
never actually run. Three habits keep them apart:

### a. Push personal logic to the YAML layer
ESPHome splits cleanly: the **component (C++) exposes capabilities**; your **automation
(YAML) consumes them**. Anything expressible as a template sensor, automation, or lambda
lives in `device/` + `packages/` — personal *by location*, and physically unable to overlap
component C++. Default question for any personal need: *"Can this live in YAML against
something the component exposes?"* Usually yes → zero overlap by construction.

### b. When the component must change, make the change *general* (a contrib)
When personal work needs something the C++ doesn't provide (data to expose, a control it
lacks), don't add a personal-specific edit inside a core function. Add a **general
capability you'd be happy to contribute** — a new sensor, a service/number, a null-safe
callback hook — commit it `contrib:`, and consume it from YAML. The payoff is exactly the
confidence you want: that capability runs in your live build every day (genuinely exercised)
*and* it's contributable *and* it's a general line in core, not a personal one.

### c. Keep a clean file partition
- **Core / contributable** (only `contrib:` commits touch these): the existing component
  files — `ble_connection_manager`, `session`, `auth`, `telemetry_service`, `transport`,
  `control_service`, `alpha_hwr` core — plus any *general* new sensor/service.
- **Personal** (only `local:` commits): `device/`, `packages/`, and — for the rare
  truly-personal C++ — its **own new file** in the component (a `local:` file, excluded from
  extraction), with core reaching it only through a general, null-safe hook. **Never put
  personal-specific lines inside a core function.**

Hold that partition and the contrib extraction is the core files exactly as they run in your
build.

### Worked decisions
- **Pump speed control** (a *deliberate, usage-model divergence* — stays `local:` by design,
  **not** a contribution as-is): upstream treats the pump as the master store of the intended
  speed (the UI just tells the pump to change its stored value — mirroring the phone app).
  That model is **right for one usage pattern and wrong for another**: if you use the pump's
  built-in scheduler (the pump autonomously turns on at set times), the pump *must* store the
  speed to run at, so pump-as-master is necessary — and upstream's choice makes sense for its
  phone-app-derived, autonomous-operation audience. This setup is HA-driven instead (HA
  decides on/off), so HA-as-master fits better, and keeping the desired speed in HA also
  survives pump resets/replacements. The practical trigger to diverge was simpler, though:
  upstream's speed path is currently *broken* (turn-on sends a hard-coded default; the
  setpoint isn't factored in; adjusting speed also turns the pump on), and a quick `local:`
  patch — keep desired speed in HA, inject it on every turn-on — was expedient to get running
  so the BLE work could proceed. A real contribution here would be to **support both models**
  (fix the broken setpoint path / make the turn-on speed source configurable), which needs
  maintainer buy-in — and they're currently inactive. So it stays personal for now, cleanly
  isolated in `control_service`. See §8 for keeping this divergence across upstream syncs, and
  `device/ARCHITECTURE_NOTES.md` for the maintainer-facing architecture discussion.
- **New monitoring sensors** (next step): expose each as a **general `contrib:` sensor** in
  the component (it runs in prod = tested); put the monitoring/alerting **automation in
  YAML** (personal). Most "new sensors" become contributions you're already running.

### Honest limits
- You won't hit *literally* zero overlap forever; a genuinely personal C++ behavior with no
  general framing will occasionally need a seam. Drive those near-zero with the hook
  pattern; accept the rare one as a known `local:`-in-core spot to hand-resolve at
  extraction (§6, §7).
- Production proves the code *runs*, but not that the component **compiles standalone** (a
  core file could accidentally lean on a personal symbol). Cover that with a single
  `esphome compile` of the `contrib/<topic>` branch against a minimal throwaway config at PR
  time, plus the maintainer's CI as a backstop. Not an ongoing harness — just a pre-submit
  smoke check.

---

## 2. The one rule: a commit is ONE track, never both

Every commit is either contribution-worthy **or** personal — never mixed, even when both
edits are in the same file in the same sitting. Hold this line and you can always rebuild a
clean contribution later by cherry-picking just the contribution commits. Break it once
(one commit that both "adds a personal sensor" *and* "fixes a core bug") and that bug fix is
welded to personal code — extraction becomes manual surgery.

---

## 3. How to label commits (the exact convention)

**Every commit subject starts with one of two tags.** The tag (`contrib:` / `local:`) is
the only part that's load-bearing — it's what makes the two tracks sortable.

### `contrib:` — belongs upstream
Form: `contrib: <summary>`.

The summary is *optionally* written in **upstream's Conventional Commits style**
(`<type>(alpha_hwr): …`, where `<type>` is `fix` `feat` `refactor` `docs` `test` `chore`).
That type is **not required** — it does nothing for sortability and only matters as polish
the maintainer's project uses. Writing it now just means extraction is a clean prefix-strip;
if you skip it, write the proper message at extraction time instead. Don't agonize over
`fix` vs `feat`: "was broken, now works" → `fix`; "new capability" → `feat`; decide at PR
time if unsure.

Examples:
```
contrib: fix(alpha_hwr): serialize subscribe behind encryption-complete
contrib: fix(alpha_hwr): cancel in-flight handshake work on disconnect
contrib: feat(alpha_hwr): expose pump link status sensor
contrib: add speed setpoint service        # type omitted — fine; reword at extraction
```

### `local:` — personal only
Anything that should never go upstream: `device/` files, `packages/` tweaks for this setup,
personal features, notes. Free-form after the tag (no type — it never goes upstream).

Examples:
```
local: add Pairing Detail sensor + Pump Link Status enum
local: assert explicit speed setpoint on pump start
local: point packages at local component source for building
local: update DESIGN_NOTES with Test C results
```

### Which tag? The litmus test
> "Would a stranger with a different ALPHA HWR pump and no Home Assistant want this?"

Yes → `contrib:`. No → `local:`. When unsure, default to `local:` — cheap to promote later,
whereas a personal detail in a PR is noise the maintainer has to catch.

### Finding them later
```bash
# contribution commits since upstream, oldest-first (cherry-pick order):
git log --grep '^contrib:' upstream/main..bt_issues --reverse --oneline
# personal commits:
git log --grep '^local:' upstream/main..bt_issues --reverse --oneline
```

---

## 4. Splitting same-file edits into separate commits

When one editing session touched both tracks in the same file, stage them separately with
**patch mode** (`-p` lets you pick individual hunks):

```bash
git add -p components/alpha_hwr/ble_connection_manager.cpp
#   y = stage this hunk, n = skip, s = split into smaller hunks, ? = help
#   Pick ONLY the contribution hunks, then:
git commit -m "contrib: fix(alpha_hwr): <summary>"

git add -p components/alpha_hwr/ble_connection_manager.cpp
#   Now pick the personal hunks:
git commit -m "local: <summary>"
```

If two changes are tangled within the *same lines*, they're not really separable — that's
the situation §1 is meant to prevent; if it happens anyway, see §6.

---

## 5. The inevitable detour: hitting a core bug while doing personal work

You're deep in a `local:` feature and discover a core bug or missing scaffolding that blocks
you. This is normal. The right move is usually §1b: fix it as a **general `contrib:`
capability** and build the personal part on top in YAML. Capture the fix as its **own
`contrib:` commit** right then — don't fold it into the personal feature commit.

If your personal work is uncommitted WIP, peel the fix out cleanly:

```bash
# Option A — stash personal WIP, make the clean fix, come back:
git stash push -m "personal WIP"
# ...write/verify just the core fix...
git add -p <file>          # stage only the fix
git commit -m "contrib: fix(alpha_hwr): <summary>"
git stash pop              # resume personal work

# Option B — if both are already in your working tree, just split with add -p (see §4):
git add -p <file>          # stage only the fix hunks
git commit -m "contrib: fix(alpha_hwr): <summary>"
# (personal hunks remain for a later `local:` commit)
```

Result: the fix is a standalone `contrib:` commit, separable later, even though you found it
mid-personal-work. It's fine that `contrib:` and `local:` commits end up interleaved in
`bt_issues` history — extraction sorts by **label**, not by order.

---

## 6. Fallback: when two changes genuinely must touch the same code

§1 should keep this rare. When it does happen (a personal change and a contribution must
touch the **same region** — e.g. a personal hook in a function a contribution also rewrites):

**Do the contribution change first and commit it; build the personal change on top.**

That puts the contribution commit "underneath," so it cherry-picks onto a clean upstream
branch with no conflict, and the personal commit carries the dependency. The reverse
(personal underneath, contribution layered on top) is what causes extraction conflicts.

Keep contribution touches in shared files **additive and minimal** (new members, new calls,
new enum values) rather than refactoring shared functions for personal convenience — additive
hunks live in different regions and stay separable almost for free.

Caveat: ordering keeps the cherry-pick *clean*, but the extracted "upstream + contrib only"
state still wasn't built/run as such → it needs the §1 compile smoke check before the PR.

---

## 7. Extracting a contribution (when ready to PR)

```bash
git fetch upstream
git switch -c contrib/<topic> upstream/main      # clean branch off latest upstream

# list the contrib commits to take, oldest-first:
git log --grep '^contrib:' --reverse --format='%H  %s' <base>..bt_issues
#   <base> = upstream/main for the first contribution, or the last-extracted point

# cherry-pick them in that order:
git cherry-pick <sha1> <sha2> <sha3>

# (if you used the conventional-commits form) strip the "contrib: " prefix:
git rebase -i upstream/main
#   mark each commit 'reword', delete the leading "contrib: " from each subject

# Validate — this combination has never been built as a unit (see §1 limits):
esphome compile <minimal throwaway config pointing at this branch's components/>

git push origin contrib/<topic>
# open the PR: base = eman/esphome-alpha-hwr:main, head = your fork:contrib/<topic>
```

**Why the compile step matters:** the extracted branch is the core code your house runs
(so its behavior is already exercised), *but* it's now compiled without your personal files —
the one thing production can't prove is that it builds standalone. The compile catches a core
file accidentally referencing a personal symbol. The maintainer's CI is the backstop.

Review-requested edits during the PR happen **on the `contrib/<topic>` branch** (push to
update the PR). If you also want those edits in your running device, cherry-pick them back to
`bt_issues` (as `contrib:` commits) afterward.

Stacked contributions (e.g. race #2, which shares files with and depends on contrib1): branch
the second one off the first (`git switch -c contrib/race2 contrib/<topic>`), or off
`upstream/main` *after* contrib1 has merged upstream.

---

## 8. Syncing with upstream later (after your PR merges and upstream keeps moving)

Scenario: the maintainer merges your PR, then does more work (e.g. fixes broken sensors and
controls). You want those changes **without losing your personal work**.

### Step 1 — refresh your mirror `main`
```bash
git fetch upstream
git switch main
git merge --ff-only upstream/main     # fast-forward; works because you never commit to main
git push origin main
```
`main` now contains the latest upstream — including your merged contribution **and** their
new work.

### Step 2 — replay your personal work on top
```bash
git switch bt_issues
git rebase -i main
```
In the editor:
- **Delete** the lines for commits that are now upstream — your contribution commits that got
  merged. (If the maintainer *squash-merged*, your originals won't auto-drop because the
  squashed commit has a different identity, so you remove them by hand here. This is the
  important bit that avoids re-applying already-merged changes and hitting conflicts.)
- **Keep** your `local:` commits and any `contrib:` commits not yet merged.
- Save.

### Step 3 — resolve conflicts, preferring upstream where it supersedes a hack
Expect conflicts specifically where **upstream's fixes overlap your `local:` changes** —
likely, since upstream may fix the very sensors/controls you hacked around. For each:
- If upstream now does properly what your `local:` hack did as a shortcut → **take upstream's
  version and drop your hack** (resolve by deleting your change).
- If your personal change is still additive/independent → keep it on top.
- If your change is a **deliberate divergence you stand by** (your usage model differs from
  upstream's — e.g. the speed-control architecture in §1) → **keep yours**, even if upstream
  "fixed" their version. Re-evaluate only if upstream later adopts or supports your model.
```bash
# during the rebase, for each conflicted file:
#   edit to resolve, then:
git add <file>
git rebase --continue
# abort and rethink at any point with:  git rebase --abort
```
(This conflict pain is exactly what §1 minimizes: the more personal logic lived in YAML and
the fewer personal lines sat inside core files, the fewer conflicts here.)

### Step 4 — publish your updated personal branch
Rebasing rewrites history, so the push needs a force (safe here — `bt_issues` is yours):
```bash
git push --force-with-lease origin bt_issues
```

Result: `bt_issues` = latest upstream + only your still-relevant personal/unmerged work, with
the already-contributed parts now coming *from* upstream instead of duplicated.

> Tip: if a long rebase makes you re-resolve the same conflict repeatedly, enable
> `git config rerere.enabled true` once — Git remembers resolutions and replays them.

---

## 9. Quick reference

```bash
# Structure first (§1): personal logic -> YAML in device/packages; component changes -> general/contributable.
# Then commit tags (subject prefix, pick one — never mix in one commit):
#   contrib: <summary>     -> belongs upstream  (optional: fix/feat(alpha_hwr): ...)
#   local:   <summary>     -> personal only

# Split a mixed working tree into per-track commits:
git add -p <file>     # pick hunks -> commit with the matching tag, repeat

# See each track's commits since upstream:
git log --grep '^contrib:' upstream/main..bt_issues --reverse --oneline
git log --grep '^local:'   upstream/main..bt_issues --reverse --oneline

# Extract a contribution:
git fetch upstream
git switch -c contrib/<topic> upstream/main
git cherry-pick <contrib shas, oldest first>
git rebase -i upstream/main        # reword: strip "contrib: " if present
esphome compile <minimal config>   # standalone-build smoke check
git push origin contrib/<topic>

# Sync with upstream after merge:
git fetch upstream
git switch main && git merge --ff-only upstream/main && git push origin main
git switch bt_issues && git rebase -i main     # drop merged commits, keep local:
git push --force-with-lease origin bt_issues
```

---

### One-line mental model
**Structure so the tracks never touch the same code (personal → YAML, core changes →
general/contributable); tag every commit `contrib:` or `local:`; `bt_issues` is everything
you run, `main` mirrors upstream, contributions are clean cherry-pick extractions onto
`contrib/*`. Because the extracted core code is what your house runs, production is the
test — with a standalone compile as the only added check.**

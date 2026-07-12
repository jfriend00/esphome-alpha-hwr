# ALPHA HWR HA Test Harness -- Setup

This document describes how to install and run the Home Assistant test harness
for the `alpha_hwr` ESPHome component. It is written to be followed on any
Home Assistant installation and uses Home Assistant's native paths (`/config/...`).

At this stage the harness is a **skeleton**: a single pyscript service that logs
a line when called, used to verify the install and invocation path end to end
before any pump-driving logic is added.

---

## 1. Prerequisites

- A running Home Assistant instance with the **[pyscript](https://github.com/custom-components/pyscript)**
  integration installed (via HACS) and enabled.
- The `alpha_hwr` ESPHome component installed and its pump controller online
  (not required for the skeleton ping, but required for later test stages).

---

## 2. Layout

In this repository the harness lives under `tools/ha-test/`, mirroring the
Home Assistant pyscript layout so deployment is a straight directory copy:

```
tools/ha-test/
├── SETUP.md                              <- this file
└── pyscript/
    └── apps/
        └── alpha_hwr_test/
            └── __init__.py               <- the app (currently: one ping service)
```

The `pyscript/apps/alpha_hwr_test/` subtree maps directly onto the Home
Assistant config:

```
tools/ha-test/pyscript/apps/alpha_hwr_test/   ->   /config/pyscript/apps/alpha_hwr_test/
```

---

## 3. Deploy the app

The app must end up at:

```
/config/pyscript/apps/alpha_hwr_test/
```

### Option A -- Manual copy (any platform)

Copy the whole `alpha_hwr_test/` directory (so future multi-file additions come
along automatically) from the repo into your Home Assistant config:

```
tools/ha-test/pyscript/apps/alpha_hwr_test/   ->   /config/pyscript/apps/alpha_hwr_test/
```

### Option B -- `deploy.bat` (Windows, optional)

`tools/ha-test/deploy.bat` automates the copy on Windows, where the Home
Assistant `/config` folder is typically reached via a mapped network drive. It
copies **only** the `alpha_hwr_test` app directory and never touches any other
file under `/config/pyscript/`.

**One-time setup** -- tell it where your Home Assistant config root is (the
folder that contains `configuration.yaml`) by setting an environment variable:

```bat
setx ALPHA_HWR_TEST_HA_CONFIG "X:\path\to\config"
```

Open a new terminal afterward so the variable is picked up. (You can skip this
and pass the path as an argument instead -- see below.)

**Deploy:**

```bat
tools\ha-test\deploy.bat             REM uses ALPHA_HWR_TEST_HA_CONFIG
tools\ha-test\deploy.bat X:\config   REM or pass the config root explicitly
tools\ha-test\deploy.bat /L          REM dry run: list what WOULD change, copy nothing
tools\ha-test\deploy.bat --no-reload REM deploy but do not reload afterward
```

Running a dry run (`/L`) first is recommended. The script validates that the
target exists and looks like a Home Assistant config (has `configuration.yaml`)
before copying, and aborts otherwise.

**Optional auto-reload.** After a successful (non-dry-run) deploy, deploy.bat
can reload the app for you by calling `pyscript.reload` over the REST API. It
uses a plain reload (no `global_ctx`): pyscript reloads only the files that
*changed* (plus anything that imports them), so your other pyscript scripts --
being unchanged -- are left alone. Enable it by setting two more environment
variables:

```bat
setx ALPHA_HWR_TEST_HA_URL   "http://your-ha-host:8123"
setx ALPHA_HWR_TEST_HA_TOKEN "your-long-lived-token"
```

(See Section 7 for creating and storing the token.) With both set, the dev loop
becomes: edit -> `deploy.bat` -> the app is deployed *and* reloaded. If either
variable is unset, or you pass `--no-reload`, the script skips the reload and
prints a reminder to run `pyscript.reload` manually.

> A cross-platform equivalent (e.g. `deploy.sh` / `deploy.py`) may be added
> later for macOS/Linux; for now Option A covers those platforms.

---

## 4. Configure the app

pyscript **will not load an app unless it has a configuration entry**, even an
empty one. Add the following under the `pyscript:` block in
`/config/configuration.yaml`:

```yaml
pyscript:
  apps:
    alpha_hwr_test:
      controller_name: your_controller_name
```

- `controller_name` is the ESPHome node/device name that prefixes this
  component's entities on **your** install. For example, if your pump-enabled
  switch is `switch.recirc_controller_pump_enabled`, then
  `controller_name: recirc_controller`.
- The skeleton only echoes this value back in its log line; later stages use it
  to derive the full entity IDs the harness reads and controls.

---

## 5. Load the app

Apply the new files and config without a full restart by calling the pyscript
reload service. `pyscript.reload` re-reads the `pyscript:` section of
`configuration.yaml` itself, so it both picks up the new `apps:` entry **and**
loads the app in one step -- no separate config reload or restart is needed.

### Reload just this app (recommended)

Reloading only `alpha_hwr_test` leaves all your other pyscript scripts and
automations untouched:

1. Go to **Developer Tools -> Actions** (Services).
2. Select **`pyscript.reload`**.
3. Switch to **YAML mode** (or use the data field) and supply:
   ```yaml
   global_ctx: apps.alpha_hwr_test
   ```
4. Click **Perform action**.

On success you will see a log line noting that `__init__.py` was loaded (see
Section 8 for where to view logs).

### Other reload options

- **Plain reload** -- `pyscript.reload` with no data reloads only the scripts,
  apps, and modules that have *changed* since they were last loaded, so
  unchanged automations are still left alone.
- **Full reload** -- add `global_ctx: '*'` to force-reload every script, app,
  and module.
- **Restart** -- a full Home Assistant restart also works and is the guaranteed
  fallback if a reload does not pick up the app for any reason.

---

## 6. Test from the Home Assistant UI

1. Go to **Developer Tools -> Actions** (Services).
2. Find and select **`pyscript.alpha_hwr_test_ping`** ("ALPHA HWR test ping").
3. Click **Perform action**.

Then check the log output (see Section 8). You should see a line like:

```
alpha_hwr_test: ping -- harness skeleton alive; controller_name=your_controller_name
```

---

## 7. Test from a URL (REST API)

pyscript services are exposed as Home Assistant services, which are callable
over the REST API. This is the basis for driving the harness from an external
tool later.

### Get a token

Create a **long-lived access token** yourself in Home Assistant under
**your profile -> Security -> Long-lived access tokens**.

A long-lived token does not expire and grants full API access, so **treat it
like a password**: never commit it, never paste it into shared output, and if
it is ever exposed, revoke it in the same screen and mint a new one.

### Store the token (recommended: environment variable)

Keep the token out of the repo, out of any committed file, and off your command
line / shell history. The convention here mirrors `ALPHA_HWR_TEST_HA_CONFIG`:
store it in an environment variable named **`ALPHA_HWR_TEST_HA_TOKEN`**.

```bat
setx ALPHA_HWR_TEST_HA_TOKEN "your-long-lived-token"
```

Open a new terminal afterward so the variable is available. Referencing the
variable (rather than pasting the token inline) also keeps it out of your shell
history.

> Trade-off: on Windows `setx` stores the value in your user registry in
> plaintext (readable by processes running as you) -- fine for a home-lab tool.
> For stronger protection use Windows Credential Manager or PowerShell's
> SecretManagement/SecretStore module. If you ever keep the token in a file
> instead, store it outside the repo or add it to `.gitignore`.

### Call the service

```powershell
# PowerShell
curl -Method POST `
  -Headers @{ Authorization = "Bearer $env:ALPHA_HWR_TEST_HA_TOKEN" } `
  http://<your-ha-host>:8123/api/services/pyscript/alpha_hwr_test_ping
```

```bat
REM cmd
curl -X POST ^
  -H "Authorization: Bearer %ALPHA_HWR_TEST_HA_TOKEN%" ^
  http://<your-ha-host>:8123/api/services/pyscript/alpha_hwr_test_ping
```

```bash
# bash / macOS / Linux (token in ALPHA_HWR_TEST_HA_TOKEN)
curl -X POST \
  -H "Authorization: Bearer ${ALPHA_HWR_TEST_HA_TOKEN}" \
  http://<your-ha-host>:8123/api/services/pyscript/alpha_hwr_test_ping
```

A successful call returns HTTP 200. Confirm the same log line appears as in
Section 6.

---

## 8. Viewing the log output

The service logs at `info` level. View it either way:

- **Settings -> System -> Logs** (widen the level to include info if needed), or
- The Home Assistant log file at `/config/home-assistant.log`.

Log entries are tagged with the source
`custom_components.pyscript.file.alpha_hwr_test.alpha_hwr_test_ping`.

If you do not see `info`-level lines, ensure pyscript's log level is not set
above `info` in your logger configuration.

---

## Status / next steps

- [x] Skeleton: single logging service, callable from UI and REST.
- [ ] Entity registry (logical name -> domain + leaf) and resolver using
      `controller_name`.
- [ ] Snapshot / restore of pump state with guaranteed restore.
- [ ] First real test: start pump, verify running, record speed, stop, verify
      stopped, report.

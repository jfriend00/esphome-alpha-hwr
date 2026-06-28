# Build & Flash Notes — recirc-controller (ESPHome)

Quick reference for building, flashing, and logging this node. Future-me (or a fresh
terminal) starts here.

**Toolchain (confirmed 2026-06-28):**
- ESPHome **2026.6.2** in a Python venv at repo-root **`.venv`** (Python 3.14.6).
- Device: `recirc-controller` @ **10.10.3.187**, OTA over **Ethernet (PoE)** — no WiFi, and
  no USB needed for normal flashing.
- Config file: **`device/recirc-controller.yaml`** — run `esphome` commands from `device/`
  (so the build dir lands in `device/.esphome`, matching how it's always been built).

---

## 1. Initialize the shell (the step that's easy to forget)

Every new terminal needs the venv activated first — the prompt then shows `(.venv)`.

**PowerShell** (paths relative to repo root):
```powershell
.\.venv\Scripts\Activate.ps1
```
If PowerShell refuses with *"running scripts is disabled on this system"*, allow it for
just this session, then activate:
```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy RemoteSigned
.\.venv\Scripts\Activate.ps1
```

**cmd.exe:** `.venv\Scripts\activate.bat`
**Git Bash:** `source .venv/Scripts/activate`

Leave the venv with `deactivate`.

### If the venv is missing or broken (fresh machine, etc.)
```powershell
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install esphome==2026.6.2     # drop the ==pin for the latest release
```
There's no `requirements.txt` — `esphome` is the only dependency.

---

## 2. Everyday commands (run from `device/`)

```
esphome config  recirc-controller.yaml    # validate YAML + packages + !extend merges, NO build
esphome compile recirc-controller.yaml    # compile firmware only
esphome run     recirc-controller.yaml    # compile + upload (OTA) + stream logs  <- the usual one
esphome logs    recirc-controller.yaml    # just stream logs (over the network/API)
esphome upload  recirc-controller.yaml    # upload an already-built binary
esphome clean   recirc-controller.yaml    # wipe build artifacts (when a build goes weird)
```

- **`config`** is the fast pre-flight: it resolves the package includes and the `!extend`
  overrides and fails loudly on any error, without building. Run it after editing YAML.
- **`run`** auto-discovers the node on the LAN and flashes over OTA — no cable needed.
- From the repo root instead of `device/`, append the path:
  `esphome run device/recirc-controller.yaml` (the `../packages` includes still resolve,
  since `!include` is relative to the YAML file, but the build dir then lands at the root).
- **Secrets:** needs `device/secrets.yaml` providing `recirc_api_key` and
  `esp32_ota_password` (referenced via `!secret`).

---

## 3. Capture logs to a file AND watch the console (tee)

**PowerShell:**
```powershell
esphome logs recirc-controller.yaml 2>&1 | Tee-Object -FilePath stage1.log
```
**Git Bash / Linux:**
```bash
esphome logs recirc-controller.yaml 2>&1 | tee stage1.log
```
`2>&1` folds stderr into stdout so warnings/errors are captured too, not just printed.
The same pattern works on `run` (captures the compile + upload + log stream):
```powershell
esphome run recirc-controller.yaml 2>&1 | Tee-Object -FilePath run.log
```

**Caveat:** the network/API logger **drops during an ESP32 reboot**, so a reboot-triggered
BLE sequence is *not* fully capturable over the network. A **pump power-cycle** (ESP32 stays
up) **is** capturable. To capture across a reboot, use USB serial (next section).

---

## 4. USB serial logs (only when you must capture a reboot)

Plug in USB, find the COM port (Device Manager → Ports), then:
```powershell
esphome logs recirc-controller.yaml --device COM5
```
Full logging over USB-CDC needs `logger: hardware_uart: USB_CDC` in the YAML — the default
routes full logs to UART0, so the USB port shows only a sparse stream until that's set.
(See `DESIGN_NOTES.md` → "capturing the destroying cycle.")

---

## 5. Filtering a captured log (PowerShell)

BLE connect / readiness-gate / pairing / link-status sequence:
```powershell
Select-String -Path stage1.log -Pattern "Bond state|Service discovery complete|Pump ready|Requesting encryption|Auth mode|0x61|0x52|SEC_REQ|Pump link status|Session:.*READY"
```
A clean **bonded reconnect** reads, in order: `BONDED` → `Service discovery complete` →
`Pump ready (discovery ok). Requesting encryption` → `Auth mode: 0x09` →
`Session: AUTHENTICATING -> READY`, with **no `0x61` and no `0x52`**. See `DESIGN_NOTES.md`
for the annotated reference captures.

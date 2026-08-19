@echo off
setlocal EnableExtensions

REM ============================================================================
REM  deploy.bat -- copy the alpha_hwr_test pyscript app into a Home Assistant
REM  config's pyscript/apps folder.
REM
REM  Usage:
REM    deploy.bat                 Deploy (env var target) then reload the app
REM    deploy.bat <config-root>   Deploy to the given HA config root (overrides env)
REM    deploy.bat /L              Dry run -- list what WOULD change, copy nothing
REM    deploy.bat /L <config-root>  Dry run against an explicit config root
REM    deploy.bat --no-reload     Deploy but do NOT reload the app afterward
REM  (flags may be combined and given in either order before the target.)
REM
REM  The target must be a Home Assistant config ROOT (the folder containing
REM  configuration.yaml). The script appends pyscript\apps\alpha_hwr_test itself.
REM
REM  RELOAD: after a successful (non-dry-run) deploy, the script calls
REM  pyscript.reload with NO global_ctx -- a plain reload, which reloads every
REM  CHANGED file (plus its dependents) and leaves unchanged contexts alone.
REM  General (independent of our file names/structure) and surgical (a deploy
REM  only changes our files; this is NOT the "*" force-reload-everything form).
REM  Requires ALPHA_HWR_TEST_HA_URL (e.g. http://homeassistant.local:8123) and
REM  ALPHA_HWR_TEST_HA_TOKEN. If either is unset, or --no-reload is given, it
REM  just prints a manual reminder instead.
REM
REM  SAFETY: only ever writes inside ...\pyscript\apps\alpha_hwr_test. It copies
REM  into that single app directory and never touches any other file under
REM  pyscript\. (Purge is currently disabled -- see the copy step below.)
REM ============================================================================

set "APP_NAME=alpha_hwr_test"
set "SRC=%~dp0pyscript\apps\%APP_NAME%"

REM --- Optional leading flags (either order): /L | --dry-run, --no-reload ----
set "DRYRUN="
set "NORELOAD="
REM two passes so the two flags may appear in either order before the target
if /I "%~1"=="/L"          ( set "DRYRUN=1"   & shift )
if /I "%~1"=="--dry-run"   ( set "DRYRUN=1"   & shift )
if /I "%~1"=="--no-reload" ( set "NORELOAD=1" & shift )
if /I "%~1"=="/L"          ( set "DRYRUN=1"   & shift )
if /I "%~1"=="--dry-run"   ( set "DRYRUN=1"   & shift )
if /I "%~1"=="--no-reload" ( set "NORELOAD=1" & shift )

REM --- Resolve target: explicit arg overrides the env var -------------------
if not "%~1"=="" (
    set "HA_CONFIG=%~1"
) else (
    set "HA_CONFIG=%ALPHA_HWR_TEST_HA_CONFIG%"
)

if not defined HA_CONFIG (
    echo ERROR: No target specified.
    echo   Set ALPHA_HWR_TEST_HA_CONFIG to your Home Assistant config root
    echo   ^(the folder containing configuration.yaml^), or pass it as an argument:
    echo       deploy.bat ^<path-to-config^>
    exit /b 1
)

REM --- Strip a trailing backslash so path building is consistent ------------
if "%HA_CONFIG:~-1%"=="\" set "HA_CONFIG=%HA_CONFIG:~0,-1%"

REM --- Validate the source app directory -----------------------------------
if not exist "%SRC%\" (
    echo ERROR: Source app directory not found:
    echo     "%SRC%"
    exit /b 1
)

REM --- Validate the target looks like a Home Assistant config ---------------
if not exist "%HA_CONFIG%\" (
    echo ERROR: Target does not exist: "%HA_CONFIG%"
    exit /b 1
)
if not exist "%HA_CONFIG%\configuration.yaml" (
    echo ERROR: "%HA_CONFIG%" does not look like a Home Assistant config directory
    echo        ^(no configuration.yaml found there^).
    exit /b 1
)
if not exist "%HA_CONFIG%\pyscript\" (
    echo NOTE: "%HA_CONFIG%\pyscript" does not exist yet -- is pyscript installed?
    echo       It will be created, but the app will not run until pyscript is set up.
)

REM --- Compute destination and guard it ------------------------------------
set "DEST=%HA_CONFIG%\pyscript\apps\%APP_NAME%"

REM Belt-and-suspenders: a blank APP_NAME would make DEST point at the apps
REM PARENT folder; if /MIR (purge) is ever re-enabled that would delete ALL
REM apps. Refuse if it is empty.
if not defined APP_NAME (
    echo ERROR: APP_NAME is empty -- refusing to deploy.
    exit /b 1
)

echo Deploying %APP_NAME%
echo   from: %SRC%
echo   to:   %DEST%
if defined DRYRUN echo   MODE: DRY RUN ^(no files will be changed^)
echo.

REM --- Copy. Writes only inside the single app directory. -------------------
REM NOTE: purge is temporarily DISABLED pending review (John, 2026-07-11).
REM   /MIR = /E + /PURGE  (mirror; deletes dest files not present in source)
REM   /E   = copy subdirs incl. empty; does NOT delete anything in dest
REM The commented /MIR lines are the originals -- re-enable to restore purge.
REM /XD profiles excludes the runtime "profiles" folder from copy/purge so saved
REM profiles are never touched by a deploy (even if /MIR is ever re-enabled).
if defined DRYRUN (
    REM robocopy "%SRC%" "%DEST%" /MIR /XD profiles /L
    robocopy "%SRC%" "%DEST%" /E /XD profiles /L
) else (
    REM robocopy "%SRC%" "%DEST%" /MIR /XD profiles /NFL /NDL /NJH /NJS /NP
    robocopy "%SRC%" "%DEST%" /E /XD profiles /NFL /NDL /NJH /NJS /NP
)
set "RC=%ERRORLEVEL%"

REM robocopy exit codes: 0-7 = success (0 = nothing to do), >=8 = failure.
if %RC% GEQ 8 (
    echo.
    echo ERROR: robocopy failed with exit code %RC%.
    exit /b 1
)

echo.
if defined DRYRUN (
    echo Dry run complete -- nothing was changed.
    exit /b 0
)

echo Done. Deployed %APP_NAME% to "%DEST%".

REM --- Reload the app in Home Assistant ------------------------------------
REM Scoped pyscript.reload (only apps.%APP_NAME%) via REST. Needs URL + token.
if defined NORELOAD (
    echo Skipping reload ^(--no-reload^) -- run pyscript.reload yourself to load changes.
    exit /b 0
)

set "CANRELOAD="
if defined ALPHA_HWR_TEST_HA_URL if defined ALPHA_HWR_TEST_HA_TOKEN set "CANRELOAD=1"
if not defined CANRELOAD (
    echo Reminder: run the pyscript.reload service in Home Assistant to load changes.
    echo   ^(Set ALPHA_HWR_TEST_HA_URL and ALPHA_HWR_TEST_HA_TOKEN to auto-reload from here.^)
    exit /b 0
)

REM strip a trailing slash from the URL so path building is clean
set "HA_URL=%ALPHA_HWR_TEST_HA_URL%"
if "%HA_URL:~-1%"=="/" set "HA_URL=%HA_URL:~0,-1%"

REM Scoped reload of THIS app only (global_ctx=apps.%APP_NAME%): pyscript tears
REM down and rebuilds the app's context as a single unit, so __init__.py runs
REM exactly ONCE. A plain reload (empty "{}") instead re-runs __init__ once per
REM changed file it imports -- harmless for the app's idempotent @service
REM handlers, but it double-registered a persistent @event_trigger during
REM development. Scoped avoids that and still touches no other pyscript app.
REM
REM CAVEAT: this assumes the app is already configured AND loaded. A brand-new
REM app (first-ever "apps: alpha_hwr_test:" entry in configuration.yaml, or a
REM changed controller_name) still needs a one-time plain reload (empty "{}"),
REM full reload (global_ctx "*"), or HA restart to be picked up.
echo Reloading pyscript app %APP_NAME% via %HA_URL% (scoped reload) ...
curl -sS --fail -o nul --connect-timeout 5 -m 30 -X POST -H "Authorization: Bearer %ALPHA_HWR_TEST_HA_TOKEN%" -H "Content-Type: application/json" -d "{\"global_ctx\":\"apps.%APP_NAME%\"}" "%HA_URL%/api/services/pyscript/reload" || (
    echo WARNING: reload failed -- check URL/token, or reload manually.
    exit /b 1
)

echo Reload complete.
exit /b 0

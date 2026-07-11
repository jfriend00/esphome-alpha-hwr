@echo off
setlocal EnableExtensions

REM ============================================================================
REM  deploy.bat -- copy the alpha_hwr_test pyscript app into a Home Assistant
REM  config's pyscript/apps folder.
REM
REM  Usage:
REM    deploy.bat                 Deploy using the ALPHA_HWR_TEST_HA_CONFIG env var
REM    deploy.bat <config-root>   Deploy to the given HA config root (overrides env)
REM    deploy.bat /L              Dry run -- list what WOULD change, copy nothing
REM    deploy.bat /L <config-root>  Dry run against an explicit config root
REM
REM  The target must be a Home Assistant config ROOT (the folder containing
REM  configuration.yaml). The script appends pyscript\apps\alpha_hwr_test itself.
REM
REM  SAFETY: only ever writes inside ...\pyscript\apps\alpha_hwr_test. It copies
REM  into that single app directory and never touches any other file under
REM  pyscript\. (Purge is currently disabled -- see the copy step below.)
REM ============================================================================

set "APP_NAME=alpha_hwr_test"
set "SRC=%~dp0pyscript\apps\%APP_NAME%"

REM --- Optional dry-run flag as the first argument ---------------------------
set "DRYRUN="
if /I "%~1"=="/L"        ( set "DRYRUN=1" & shift )
if /I "%~1"=="--dry-run" ( set "DRYRUN=1" & shift )

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
if defined DRYRUN (
    REM robocopy "%SRC%" "%DEST%" /MIR /L
    robocopy "%SRC%" "%DEST%" /E /L
) else (
    REM robocopy "%SRC%" "%DEST%" /MIR /NFL /NDL /NJH /NJS /NP
    robocopy "%SRC%" "%DEST%" /E /NFL /NDL /NJH /NJS /NP
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
) else (
    echo Done. Deployed %APP_NAME% to "%DEST%".
    echo Reminder: run the pyscript.reload service in Home Assistant to load changes.
)
exit /b 0

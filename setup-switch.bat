@echo off
rem Build XI on Anything for the Nintendo Switch and put it on an SD card: double-click this file,
rem or run it from a terminal with tools\setup_switch.py's options (--game, --sd, --server; --help).
rem Needs Python 3.12+ and devkitPro (docs\platforms\switch.md, Quick start).
setlocal
cd /d "%~dp0"

set PY=
py -3.12 -c "" >nul 2>&1 && set PY=py -3.12
if not defined PY py -3 -c "import sys; sys.exit(sys.version_info < (3, 12))" >nul 2>&1 && set PY=py -3
if not defined PY python -c "import sys; sys.exit(sys.version_info < (3, 12))" >nul 2>&1 && set PY=python
if not defined PY (
    echo This setup needs Python 3.12 or newer, which is not installed.
    where winget >nul 2>&1 && (
        choice /m "Install Python 3.12 now (with winget, Windows' own installer)"
        if not errorlevel 2 (
            winget install -e --id Python.Python.3.12 --accept-package-agreements --accept-source-agreements
            py -3.12 -c "" >nul 2>&1 && set PY=py -3.12
            rem (this window's PATH is from before the install: the per-user install's own folder)
            if exist "%LOCALAPPDATA%\Programs\Python\Python312\python.exe" set PY="%LOCALAPPDATA%\Programs\Python\Python312\python.exe"
        )
    )
)
if not defined PY (
    echo Install Python 3.12 or newer from https://www.python.org/downloads/
    echo ^(tick "Add python.exe to PATH" in its installer^), then run this again.
    set STATUS=1
    goto end
)

%PY% tools\setup_switch.py %*
set STATUS=%ERRORLEVEL%

:end
rem a double-click's window stays open to read
echo %CMDCMDLINE% | find /i "/c" >nul && (echo. & pause)
exit /b %STATUS%

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
    echo Python 3.12 or newer is needed: https://www.python.org/downloads/
    echo Tick "Add python.exe to PATH" in its installer, then run this again.
    set STATUS=1
    goto end
)

%PY% tools\setup_switch.py %*
set STATUS=%ERRORLEVEL%

:end
rem a double-click's window stays open to read
echo %CMDCMDLINE% | find /i "/c" >nul && (echo. & pause)
exit /b %STATUS%

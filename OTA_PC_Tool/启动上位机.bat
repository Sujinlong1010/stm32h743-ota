@echo off
chcp 65001 >nul 2>&1
cd /d "%~dp0"
title OTA one-click upgrade tool

rem ---------------------------------------------------------------------------
rem  Console launcher: keeps the error output visible if something goes wrong.
rem  For the windowless version use the .vbs launcher.
rem ---------------------------------------------------------------------------

set "PY="
set "PYARGS="
where python >nul 2>&1 && set "PY=python"
if not defined PY (
  where py >nul 2>&1 && (set "PY=py" & set "PYARGS=-3")
)
if not defined PY (
  echo.
  echo [ERROR] Python not found. Install Python 3.8+ and tick "Add Python to PATH".
  echo.
  pause
  exit /b 1
)

echo Python : %PY% %PYARGS%
echo Running: quick_ota.py
"%PY%" %PYARGS% "%~dp0quick_ota.py"
if errorlevel 1 (
  echo.
  echo [ERROR] Launch failed. Common fixes:
  echo   1^) Missing pyserial:
  echo        "%PY%" -m pip install pyserial -i https://mirrors.aliyun.com/pypi/simple
  echo   2^) This Python has no tkinter ^(official installers include it^).
  echo.
  pause
)

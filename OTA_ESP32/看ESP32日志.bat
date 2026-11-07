@echo off
chcp 65001 >nul 2>&1
cd /d "%~dp0"
title ESP32 WiFi / OTA log

rem ---------------------------------------------------------------------------
rem  Open the PlatformIO serial monitor on the ESP32's CH343 UART port.
rem  The board exposes two COM ports:
rem     CH343 / CP2102  ("UART" silkscreen)  <- Serial() logs come out here
rem     303A:1001       ("USB"  silkscreen)  <- only useful for flashing
rem  This script picks the CH343/CP2102 one automatically.
rem ---------------------------------------------------------------------------

set "PIO=%USERPROFILE%\.platformio\penv\Scripts\pio.exe"
if not exist "%PIO%" set "PIO=pio"

echo ============================================================
echo  ESP32 serial monitor @ 115200
echo  Plug in the USB cable, then press the board's RESET button.
echo  Look for lines starting with [WiFi] / [MQTT] / [PROP] / [STM32].
echo  Quit: Ctrl+C
echo ============================================================
echo.

for /f "tokens=1" %%P in ('powershell -NoProfile -Command "Get-CimInstance Win32_PnPEntity | Where-Object { $_.Name -match 'COM\d+' -and $_.Name -match 'CH34|CP21' } | ForEach-Object { if ($_.Name -match '(COM\d+)') { $matches[1] } }"') do set "PORT=%%P"

if defined PORT (
  echo  Using %PORT%
  "%PIO%" device monitor -b 115200 -p %PORT%
) else (
  echo  [WARN] No CH343/CP2102 port found, falling back to auto-select
  "%PIO%" device monitor -b 115200
)
echo.
echo Monitor exited.
pause

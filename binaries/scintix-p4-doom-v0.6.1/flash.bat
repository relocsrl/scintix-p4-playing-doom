@echo off
rem SCINTIX P4 DOOM - flash the prebuilt binaries in this folder.
rem Usage: flash.bat COMx [full, app or erase]
rem Needs Python in PATH with esptool 5.x:  pip install "esptool>=5"
setlocal
cd /d "%~dp0"

set "PORT=%~1"
set "MODE=%~2"
if "%PORT%"=="" goto usage
if "%MODE%"=="" set "MODE=full"
if not defined ESPTOOL set "ESPTOOL=python -m esptool"
if not defined BAUD set "BAUD=115200"

set "FILES=0x2000 bootloader.bin 0x8000 partition-table.bin 0x10000 doom_p4.bin 0x410000 storage.bin"
set "EXTRA="
if /i "%MODE%"=="full" goto flash
if /i "%MODE%"=="app" (set "FILES=0x10000 doom_p4.bin" & goto flash)
if /i "%MODE%"=="erase" (set "EXTRA=--erase-all" & goto flash)
goto usage

:flash
echo Flashing SCINTIX P4 DOOM (%MODE%) on %PORT% ...
%ESPTOOL% --chip esp32p4 -p %PORT% -b %BAUD% --before default-reset --after hard-reset write-flash %EXTRA% --flash-mode dio --flash-freq 80m --flash-size 16MB %FILES%
if errorlevel 1 goto failed
echo.
echo Done. The board restarts into DOOM.
exit /b 0

:failed
echo.
echo Flashing failed. Check the COM port and that esptool 5.x is installed
echo (python -m esptool version). If it can't connect, put the board in
echo download mode (hold BOOT, tap RESET, release BOOT) and run it again.
exit /b 1

:usage
echo Usage: %~nx0 COMx [full^|app^|erase]
echo.
echo   full   bootloader + partition table + DOOM + WAD (default, first flash)
echo   app    DOOM firmware only (update: keeps WAD, Wi-Fi and settings)
echo   erase  wipe the whole flash, then full (clean board, Wi-Fi portal on boot)
echo.
echo Example: %~nx0 COM5
exit /b 1

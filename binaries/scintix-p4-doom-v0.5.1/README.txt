SCINTIX P4 DOOM v0.5.1 - prebuilt firmware
==========================================

Board: SCINTIX P4 (ESP32-P4 chip rev v3.1+, ESP32-C6), 16 MB flash.
Built from tag v0.5.1 with ESP-IDF v5.5.4.
Source: https://github.com/relocsrl/scintix-p4-playing-doom

What's new in v0.5.1
- Boots straight into DOOM (no LCD test pattern).
- Quit Game restarts the board cleanly (settings saved) instead of crashing.

Files
  bootloader.bin       0x2000
  partition-table.bin  0x8000
  doom_p4.bin          0x10000    DOOM firmware
  storage.bin          0x410000   SPIFFS image with the shareware doom1.wad (12 MB)
  flash.bat            Windows flashing script

Requirements
  Python 3 in PATH and esptool 5.x:   pip install "esptool>=5"
  Check it with:                      python -m esptool version
  Plug in the board's USB port: it shows up as "USB Serial Device (COMx)".

Flash on Windows
  flash.bat COM5          full flash: first time / new board (about 1.5 min)
  flash.bat COM5 app      firmware only: update, keeps WAD, Wi-Fi and settings
  flash.bat COM5 erase    wipe the whole flash, then full flash
  Another esptool command: set ESPTOOL=esptool  (before running flash.bat)

Flash by hand (any OS), from this folder
  Full:
    python -m esptool --chip esp32p4 -p PORT -b 115200 --before default-reset --after hard-reset write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB 0x2000 bootloader.bin 0x8000 partition-table.bin 0x10000 doom_p4.bin 0x410000 storage.bin
  Firmware only:
    python -m esptool --chip esp32p4 -p PORT -b 115200 --before default-reset --after hard-reset write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB 0x10000 doom_p4.bin
  PORT is COM5 on Windows, /dev/ttyACM0 on Linux, /dev/cu.usbmodem... on macOS.

If it can't connect: hold BOOT, tap RESET, release BOOT, then run it again.

First boot
- DOOM starts on the screen right away. Play with a USB keyboard or gamepad.
- With no Wi-Fi saved, the board opens an access point SCINTIX-P4-XXXXXX: join it,
  open http://192.168.4.1 and enter your Wi-Fi. Then open the board's IP in a browser
  for the settings page, or point an AI agent at ws://<ip>/agent (see the repo README).

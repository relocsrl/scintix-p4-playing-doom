SCINTIX P4 DOOM v0.6.2 - prebuilt firmware
==========================================

Board: SCINTIX P4 (ESP32-P4 chip rev v3.1+, ESP32-C6), 16 MB flash.
Built from tag v0.6.2 with ESP-IDF v5.5.4.
Source: https://github.com/relocsrl/scintix-p4-playing-doom

What's new in v0.6.2
- AI agent API (/agent): door_ahead now points at the middle of the door, and
  use_target says what is under the crosshair within USE reach (door / lift /
  switch) - fixes the agent pressing USE into the wall next to a door and giving
  the door up. exit_sign reports an EXIT sign readable in the view; once seen it
  is 'E' on the agent map. Only what a human player sees on screen.
- AI agent API (/agent): the depth rays stop at steps too high to climb (> 24 units)
  and report stairs (rise) and damaging floors (hurt / safe); the observation adds
  floor_damage and radsuit_tics, and a raised lift shows as door_ahead kind "lift".
- Agent automap: '#' only for what blocks walking, ':' walkable steps (stairs),
  '~' edges of nukage/slime; flat open boundaries are no longer drawn.
- Audio: if the ES8311 module (EchoBase) resets while the game runs (a dip on its
  3.3 V, a loose wire), it is set up again within 2 s instead of staying silent.
- Matching Jev agent improvements are in the repo's tools/ folder.

v0.6.1
- Fast startup: DOOM is on screen ~6 s after power-on (was ~45 s). The WAD is
  copied to PSRAM once at boot instead of being read piece by piece from SPIFFS.
- Ethernet no longer delays boot by 4 s when no cable is plugged in.

v0.6.0
- Ethernet: plug a cable into the carrier's RJ45 (e.g. Raspberry Pi CM4/CM5 IO
  board); the board gets an address by DHCP, alongside Wi-Fi.
- Includes v0.5.1: boots straight into DOOM (no LCD test pattern), and Quit Game
  restarts the board cleanly (settings saved) instead of crashing.

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
- DOOM appears on the screen ~6 s after power-on. Play with a USB keyboard or gamepad.
- Ethernet needs no setup: with a cable plugged in, the board takes an address by
  DHCP (hostname scintix-p4-xxxxxx, also in the serial log: "Ethernet up with IP").
- With no Wi-Fi saved, the board opens an access point SCINTIX-P4-XXXXXX: join it,
  open http://192.168.4.1 and enter your Wi-Fi.
- Then open the board's IP (Wi-Fi or Ethernet) in a browser for the settings page,
  or point an AI agent at ws://<ip>/agent (see the repo README).

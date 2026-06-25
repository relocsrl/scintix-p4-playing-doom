# DOOM on the SCINTIX P4 (ESP32-P4 + ESP32-C6)

> ### 🚀 The SCINTIX P4 is live on Crowd Supply!
> The board this project runs on is **crowdfunding now** —
> **[back it on Crowd Supply →](https://www.crowdsupply.com/reloc/scintix-p4)**.
> Help bring the SCINTIX P4 to life. 🎮

A port of DOOM running on the **SCINTIX P4**, our custom board based on the
**ESP32-P4** with an **ESP32-C6** wireless co-processor.

> **This is a fork.** It started from the excellent ESP32-P4 DOOM port by
> **Mazur888** — [mazur888/DOOM-working-on-ESP32-P4-C6](https://github.com/mazur888/DOOM-working-on-ESP32-P4-C6),
> which targets the Guition **JC4880P443** panel (ST7701S, 480×800). This fork
> adapts that work to the **SCINTIX P4** hardware, whose display and wiring differ.

## Target hardware (SCINTIX P4)

- **MCU**: ESP32-P4 (rev. v3.1), 32 MB hex PSRAM, 32 MB flash.
- **Wireless co-processor**: ESP32-C6 over SDIO (4-bit), Wi-Fi provided to the P4
  via `esp_hosted` / `esp_wifi_remote`.
- **Display**: 7" **1024×600 MIPI-DSI** panel driven by the **EK79007** controller
  (landscape).
- **Audio**: **ES8311** I²S codec + speaker — tested with the external
  [M5 Atomic EchoBase](https://github.com/m5stack/M5Atomic-EchoBase).
- **Controls**: wired **USB** — a PS4 (DualShock 4) gamepad **or** a standard USB
  keyboard (the C6 path is not used for input).

## What this fork changes vs. the original

- **Display bring-up via BSP.** The original hand-wrote the ST7701S/MIPI init for
  the 480×800 portrait panel. This fork delegates display setup to the Espressif
  `esp32_p4_function_ev_board` BSP (vendored under `components/`), which drives the
  EK79007 1024×600 panel — the same proven path used by the board's Brookesia demo.
- **Landscape orientation.** The EK79007 is natively landscape, so the default
  panel rotation is `0` (the original rotated 90° for its portrait panel).
- **MIPI-DSI PHY clock fix** for ESP32-P4 rev ≥ 3.0 (the legacy `PLL_F20M` source
  is invalid on this silicon; the driver default — XTAL — is used instead).
- **Hardware-accelerated rendering (PPA).** The Doom frame (320×200) is upscaled
  to 1024×600 by the ESP32-P4 **PPA** (Pixel Processing Accelerator):
  `ppa_do_scale_rotate_mirror` scales — and optionally rotates 180° — straight into
  a DPI frame buffer, then `esp_lcd_panel_draw_bitmap` performs a **zero-copy
  page-flip**. With **double buffering** (two DPI frame buffers) this is tear-free.
  This replaces the per-frame CPU scale+blit (~24.5 ms) with a ~9 ms hardware op.
- **CPU scaler fallback.** A nearest-neighbour software scaler — scaling LUTs (no
  per-pixel divide) plus identical-row `memcpy` dedup — stays in place for the
  cases the PPA fast path doesn't cover (colour adjustment, 90°/270° rotation).
- **Build/runtime tuning**: `-O2`, PSRAM @ 250 MHz, flash QIO, 1 kHz FreeRTOS tick.
- **USB keyboard support.** Besides the DualShock 4 gamepad, a standard USB
  keyboard can drive the game: arrows = move/turn, **Ctrl** = fire, **Space** =
  use/open, **Shift** = run, **Alt** (with arrows) or **`,`/`.`** = strafe,
  **`1`–`7`** = weapon select, **Esc/Enter/Tab** = menu/confirm/automap. Gamepad
  and keyboard work interchangeably, selected automatically by what's plugged in.
- **Audio (SFX).** Sound effects play through an **ES8311** codec over I²S, tested
  with the [M5 Atomic EchoBase](https://github.com/m5stack/M5Atomic-EchoBase)
  (ES8311 + NS4150B amp + PI4IOE5V6408 I/O expander): the codec clock is derived
  from SCLK (no external MCLK) and the amplifier is unmuted via the I/O expander.
  Output volume is adjustable from the web settings page. (Music is disabled — SFX only.)
- **Stability fix**: larger HTTP server task stack to avoid a stack overflow when
  serving the configuration pages.

Together these bring the game to a steady **~30 FPS** — Doom's own software renderer
(~25 ms/frame) and the 35 Hz game tick (`TICRATE`) are now the limiting factors.

## Software features (inherited)

- Wi-Fi in **AP mode + captive portal** for first-time setup.
- Web server to tweak display (rotation, colour, FPS overlay) and audio settings.
- WAD shipped in SPIFFS. WAD forked from
  [Akbar30Bill/DOOM_wads](https://github.com/Akbar30Bill/DOOM_wads).

## Build, flash and monitor

Built with **ESP-IDF v5.5.x**.

```bash
idf.py set-target esp32p4
idf.py build flash monitor
```

The WAD and config live in the SPIFFS image (`storage` partition, ~12 MB), so the
first flash takes a while.

## First boot

- A colour **test pattern** is drawn to confirm the panel and MIPI link are alive,
  then the game loads.
- If colours look swapped/inverted, adjust them in the web settings page.

## Wi-Fi portal

- On first boot the device starts an **open Access Point** (SSID
  `SCINTIX-P4-XXXXXX`, where the suffix is derived from the MAC) and a captive portal.
- Connect, enter your Wi-Fi SSID and password, and save.
- Use the serial monitor or a network scanner to find the device IP, then open it
  in a browser for the settings page.

## Links & references

- **SCINTIX P4 board** — crowdfunding on [Crowd Supply](https://www.crowdsupply.com/reloc/scintix-p4) (by RELOC s.r.l.)
- **Original DOOM ESP32-P4 port** (upstream of this fork) — [mazur888/DOOM-working-on-ESP32-P4-C6](https://github.com/mazur888/DOOM-working-on-ESP32-P4-C6)
- **doomgeneric** (portable Doom core) — [ozkl/doomgeneric](https://github.com/ozkl/doomgeneric)
- **WAD** (shareware `doom1.wad`) — [Akbar30Bill/DOOM_wads](https://github.com/Akbar30Bill/DOOM_wads)
- **Audio module** — [M5 Atomic EchoBase](https://github.com/m5stack/M5Atomic-EchoBase) (ES8311 + NS4150B)
- **ESP-IDF** v5.5.x — [espressif/esp-idf](https://github.com/espressif/esp-idf)
- **ESP-Hosted** / Wi-Fi remote (P4↔C6) — [esp-hosted](https://github.com/espressif/esp-hosted) · [esp_wifi_remote](https://components.espressif.com/components/espressif/esp_wifi_remote)
- **ESP32-P4 PPA** (hardware scaler) — [PPA API docs](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/peripherals/ppa.html)

## License

This project is released under the [GNU GPL v3.0](LICENSE).

- Original work © 2026 **Mazur888**.
- SCINTIX P4 adaptations © 2026 **RELOC s.r.l.**.

No warranty; use at your own risk.

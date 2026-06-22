# DOOM on the Scintix P4 (ESP32-P4 + ESP32-C6)

A port of DOOM running on the **Scintix P4**, our custom board based on the
**ESP32-P4** with an **ESP32-C6** wireless co-processor.

> **This is a fork.** It started from the excellent ESP32-P4 DOOM port by
> **Mazur888** — [mazur888/DOOM-working-on-ESP32-P4-C6](https://github.com/mazur888/DOOM-working-on-ESP32-P4-C6),
> which targets the Guition **JC4880P443** panel (ST7701S, 480×800). This fork
> adapts that work to the **Scintix P4** hardware, whose display and wiring differ.

## Target hardware (Scintix P4)

- **MCU**: ESP32-P4 (rev. v3.1), 32 MB hex PSRAM, 32 MB flash.
- **Wireless co-processor**: ESP32-C6 over SDIO (4-bit), Wi-Fi provided to the P4
  via `esp_hosted` / `esp_wifi_remote`.
- **Display**: 7" **1024×600 MIPI-DSI** panel driven by the **EK79007** controller
  (landscape).
- **Audio**: I²S codec + speaker.
- **Controls**: USB PS4 (DualShock 4) controller, **wired only** (the C6 path is
  not used for the gamepad).

## What this fork changes vs. the original

- **Display bring-up via BSP.** The original hand-wrote the ST7701S/MIPI init for
  the 480×800 portrait panel. This fork delegates display setup to the Espressif
  `esp32_p4_function_ev_board` BSP (vendored under `components/`), which drives the
  EK79007 1024×600 panel — the same proven path used by the board's Brookesia demo.
- **Landscape orientation.** The EK79007 is natively landscape, so the default
  panel rotation is `0` (the original rotated 90° for its portrait panel).
- **MIPI-DSI PHY clock fix** for ESP32-P4 rev ≥ 3.0 (the legacy `PLL_F20M` source
  is invalid on this silicon; the driver default — XTAL — is used instead).
- **Rendering performance work** (in progress): nearest-neighbour scaling LUTs
  replacing per-pixel integer divides, plus build tuning (`-O2`, PSRAM 250 MHz).
- **Stability fix**: larger HTTP server task stack to avoid a stack overflow when
  serving the configuration pages.

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

- On first boot the device starts an **open Access Point** and a captive portal.
- Connect, enter your Wi-Fi SSID and password, and save.
- Use the serial monitor or a network scanner to find the device IP, then open it
  in a browser for the settings page.

## License

This project is released under the [GNU GPL v3.0](LICENSE).

- Original work © 2026 **Mazur888**.
- Scintix P4 adaptations © 2026 **RELOC s.r.l.**.

No warranty; use at your own risk.

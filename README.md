# An LLM plays DOOM on the SCINTIX P4 (ESP32-P4 + ESP32-C6)

**Claude Sonnet plays real DOOM** on the **SCINTIX P4** — our custom board built
around the **ESP32-P4** with an **ESP32-C6** wireless co-processor — driving the game
over an **MCP interface**. DOOM runs on the board itself (7" 1024×600 MIPI-DSI,
hardware-scaled to ~30 FPS by the P4's PPA); a small WebSocket "lockstep" API and an
MCP server expose the running game so a large language model can play it, seeing only
what a player sees on screen.

[![Claude Sonnet plays DOOM on an ESP32-P4](https://img.youtube.com/vi/z6UT_tOvC2Q/maxresdefault.jpg)](https://youtu.be/z6UT_tOvC2Q)

▶️ **[Watch the demo](https://youtu.be/z6UT_tOvC2Q)** — Claude Sonnet playing DOOM on the board.

> **This is a fork.** It started from the excellent ESP32-P4 DOOM port by **Mazur888** —
> [mazur888/DOOM-working-on-ESP32-P4-C6](https://github.com/mazur888/DOOM-working-on-ESP32-P4-C6),
> which targets the Guition **JC4880P443** panel (ST7701S, 480×800). This fork adapts
> that work to the **SCINTIX P4** hardware (different display and wiring) and adds
> audio, USB input, and the AI/MCP agent.

## Let an AI play it (MCP)

The headline feature: the firmware exposes the *running* game to a large language model.

- **Lockstep protocol.** A `/agent` WebSocket runs the game in lockstep: send one
  action, the game advances a few tics, and you get back a **structured JSON
  observation**, then it pauses until the next action. That determinism lets the model
  reason about a frozen, coherent frame — it's also why the marine moves in little bursts.
- **MCP server.** `tools/doom_mcp_server.py` wraps that WebSocket as an
  [MCP](https://modelcontextprotocol.io) server, so any MCP-capable client (e.g. Claude
  Code) can register the game as tools — `observe`, `move_forward`, `move_back`,
  `turn_left`, `turn_right`, `strafe_left`, `strafe_right`, `fire`, `use`,
  `select_weapon`, `get_map` — and just play.
- **What the agent sees — only what's on screen.** The observation is deliberately
  limited to a human's view: a **51-ray depth fan** across the field of view,
  **on-screen enemies in line of sight** (never through walls), an **ASCII automap** of
  the walls already discovered, plus **`door_ahead`** and **`blocked`** hints. Bearings
  are `positive = right`, and the facing angle is a compass heading matching the
  north-up automap.

See **[`tools/README.md`](tools/README.md)** for the agent client, the MCP server setup
and the full protocol.

```bash
# from tools/, with a venv active and the device on your network:
claude mcp add doom-scintix-p4 -e DOOM_WS_URL=ws://<device-ip>/agent -- python tools/doom_mcp_server.py
```

> Get into a level first (New Game from the menu) before driving it — at the title
> screen there is no live player to observe.

## Target hardware (SCINTIX P4)

- **MCU**: ESP32-P4 (rev. v3.1/v3.2), 32 MB hex PSRAM, 32 MB flash.
- **Wireless co-processor**: ESP32-C6 over SDIO (4-bit); Wi-Fi/BLE provided to the P4
  via `esp_hosted` / `esp_wifi_remote`.
- **Display**: 7" **1024×600 MIPI-DSI** panel driven by the **EK79007** controller (landscape).
- **Audio**: **ES8311** I²S codec + speaker — tested with the external
  [M5 Atomic EchoBase](https://github.com/m5stack/M5Atomic-EchoBase).
- **Controls**: wired **USB** — a standard USB keyboard **or** a USB gamepad
  (DualShock 4 / generic HID). The AI plays over Wi-Fi (MCP), no controller needed.

## What this fork changes vs. the original

- **AI/MCP agent.** New `/agent` WebSocket + MCP server that lets an LLM play the game
  (see above), in `main/doom_agent.*` and `tools/`.
- **Display bring-up via BSP.** The original hand-wrote the ST7701S/MIPI init for the
  480×800 portrait panel. This fork delegates display setup to the Espressif
  `esp32_p4_function_ev_board` BSP (vendored under `components/`), which drives the
  EK79007 1024×600 panel — the same proven path used by the board's Brookesia demo.
- **Landscape orientation.** The EK79007 is natively landscape, so the default panel
  rotation is `0` (the original rotated 90° for its portrait panel).
- **MIPI-DSI PHY clock fix** for ESP32-P4 rev ≥ 3.0 (the legacy `PLL_F20M` source is
  invalid on this silicon; the driver default — XTAL — is used instead).
- **Hardware-accelerated rendering (PPA).** The Doom frame (320×200) is upscaled to
  1024×600 by the ESP32-P4 **PPA** (Pixel Processing Accelerator):
  `ppa_do_scale_rotate_mirror` scales — and optionally rotates 180° — straight into a
  DPI frame buffer, then `esp_lcd_panel_draw_bitmap` performs a **zero-copy page-flip**.
  With **double buffering** (two DPI frame buffers) this is tear-free. This replaces the
  per-frame CPU scale+blit (~24.5 ms) with a ~9 ms hardware op.
- **CPU scaler fallback.** A nearest-neighbour software scaler — scaling LUTs (no
  per-pixel divide) plus identical-row `memcpy` dedup — stays in place for the cases the
  PPA fast path doesn't cover (colour adjustment, 90°/270° rotation).
- **Build/runtime tuning**: `-O2`, PSRAM @ 250 MHz, flash QIO, 1 kHz FreeRTOS tick.
- **USB keyboard + gamepad.** A standard USB keyboard (arrows = move/turn, **Ctrl** =
  fire, **Space** = use/open, **Shift** = run, **Alt** or **`,`/`.`** = strafe,
  **`1`–`7`** = weapon select, **Esc/Enter/Tab** = menu/confirm/automap) **or** a USB
  gamepad (DualShock 4 / generic HID) drives the game — selected automatically by what's
  plugged in.
- **Audio (SFX).** Sound effects play through an **ES8311** codec over I²S, tested with
  the [M5 Atomic EchoBase](https://github.com/m5stack/M5Atomic-EchoBase) (ES8311 +
  NS4150B amp + PI4IOE5V6408 I/O expander): the codec clock is derived from SCLK (no
  external MCLK) and the amplifier is unmuted via the I/O expander. Output volume is
  adjustable from the web settings page. (Music is disabled — SFX only.)
- **Stability fix**: larger HTTP server task stack to avoid a stack overflow when
  serving the configuration pages.

Together these bring the game to a steady **~30 FPS** — Doom's own software renderer
(~25 ms/frame) and the 35 Hz game tick (`TICRATE`) are now the limiting factors.

## Software features

- Wi-Fi in **AP mode + captive portal** for first-time setup, then station mode.
- Web server to tweak display (rotation, colour, FPS overlay) and audio (volume) settings.
- WAD shipped in SPIFFS (shareware `doom1.wad`, forked from
  [Akbar30Bill/DOOM_wads](https://github.com/Akbar30Bill/DOOM_wads)).

## Audio wiring (SCINTIX P4 on a Raspberry Pi CM5 IO Board)

The SCINTIX P4 (RM-CMP4) module sits on a **Raspberry Pi CM5 IO Board**, so the
ESP32-P4 GPIOs surface on the board's **40-pin header** under the Raspberry Pi pin
names. To attach an external **ES8311** audio module (e.g. the
[M5 Atomic EchoBase](https://github.com/m5stack/M5Atomic-EchoBase)), wire:

| Signal | ESP32-P4 GPIO | CM5 IO 40-pin (RPi) |
|---|---|---|
| I²C **SDA** (codec + expander) | GPIO7 | **GPIO11** |
| I²C **SCL** | GPIO8 | **GPIO5** |
| I²S **BCLK** | GPIO12 | **ID_SD** |
| I²S **WS / LRCK** | GPIO6 | **GPIO9** |
| I²S **DOUT** (→ codec) | GPIO9 | **GPIO19** |

**Power & ground** (40-pin header): the **display** runs at **5V** (pin **2** or **4**);
the **ES8311 module** (M5 EchoBase) runs at **3.3V** (pin **1** or **17**); **GND** on
pin **39** (or any other ground pin: 6/9/14/20/25/30/34).

The ES8311 (`0x18`) and the EchoBase PI4IOE5V6408 I/O expander (`0x43`) share that I²C
bus; **no external MCLK** is required (the codec clocks off SCLK). For reference, the
display signals are routed the same way: backlight P4 `GPIO3` → RPi `GPIO27`, reset P4
`GPIO4` → RPi `GPIO22`.

## Build, flash and monitor

Built with **ESP-IDF v5.5.x**.

```bash
idf.py set-target esp32p4
idf.py build flash monitor
```

The WAD and config live in the SPIFFS image (`storage` partition, ~12 MB), so the first
flash takes a while.

## First boot

- A colour **test pattern** is drawn to confirm the panel and MIPI link are alive, then
  the game loads.
- If colours look swapped/inverted, adjust them in the web settings page.

## Wi-Fi portal

- On first boot the device starts an **open Access Point** (SSID `SCINTIX-P4-XXXXXX`,
  suffix derived from the MAC) and a captive portal.
- Connect, enter your Wi-Fi SSID and password, and save.
- Use the serial monitor or a network scanner to find the device IP, then open it in a
  browser for the settings page — or point the MCP server at `ws://<ip>/agent`.

## The board

The SCINTIX P4 (RM-CMP4) is a compact ESP32-P4 compute module in the Raspberry Pi
CM4/CM5 form factor — instant boot, hard real-time, low power, reusing the CM carrier
ecosystem.

> 🚀 **The SCINTIX P4 is crowdfunding on
> [Crowd Supply](https://www.crowdsupply.com/reloc/scintix-p4)** — see also the
> [product page @ RELOC](https://www.reloc.it/products/rm-cmp4/).

## Links & references

- **Original DOOM ESP32-P4 port** (upstream of this fork) — [mazur888/DOOM-working-on-ESP32-P4-C6](https://github.com/mazur888/DOOM-working-on-ESP32-P4-C6)
- **doomgeneric** (portable Doom core) — [ozkl/doomgeneric](https://github.com/ozkl/doomgeneric)
- **WAD** (shareware `doom1.wad`) — [Akbar30Bill/DOOM_wads](https://github.com/Akbar30Bill/DOOM_wads)
- **Audio module** — [M5 Atomic EchoBase](https://github.com/m5stack/M5Atomic-EchoBase) (ES8311 + NS4150B)
- **Model Context Protocol (MCP)** — [modelcontextprotocol.io](https://modelcontextprotocol.io)
- **ESP-IDF** v5.5.x — [espressif/esp-idf](https://github.com/espressif/esp-idf)
- **ESP-Hosted** / Wi-Fi remote (P4↔C6) — [esp-hosted](https://github.com/espressif/esp-hosted) · [esp_wifi_remote](https://components.espressif.com/components/espressif/esp_wifi_remote)
- **ESP32-P4 PPA** (hardware scaler) — [PPA API docs](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/peripherals/ppa.html)

## License

This project is released under the [GNU GPL v3.0](LICENSE).

- Original work © 2026 **Mazur888**.
- SCINTIX P4 adaptations and improvements © 2026 **RELOC s.r.l.**.

`doom1.wad` is id Software's shareware DOOM data, redistributed under its shareware
terms (not GPL). No warranty; use at your own risk.

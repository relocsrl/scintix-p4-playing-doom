# SCINTIX P4 DOOM — notes for Claude Code

DOOM on the SCINTIX P4 (RELOC RM-CMP4: ESP32-P4 + ESP32-C6 over SDIO), fork of
mazur888's ESP32-P4 port. ESP-IDF **v5.5.4**, target `esp32p4`, chip rev **v3.1+**.

- Firmware: `main/` (display via the vendored `esp32_p4_function_ev_board_noglib` BSP
  → EK79007 1024×600 + PPA scaler, USB HID input, ES8311 audio, Ethernet
  (`eth_net.c`, KSZ8091 PHY), Wi-Fi portal/web settings, `/agent` WebSocket in
  `doom_agent.c` + `doom_port.c`).
  `components/doomgeneric` is the almost untouched Doom core.
- Tools: `tools/` — `agent_client.py` (REPL), `doom_mcp_server.py` (LLM via MCP),
  `doom_jev_agent.py` (TypeSafe Jev, level 1/2), `doom_agent_sim.py` (board-free
  simulator), `tests/mock_servers.py`. Docs: `tools/README.md`.
- Work in progress and everything learned about the agent/firmware:
  **`docs/jev-agent-notes.md`** — read it first when working on the agent.

Environment on Andrea's PC (Windows):
- IDF: `C:\Espressif\_idf\v5.5.4\esp-idf\export.ps1`; flash app only with
  `idf.py -p COMx build app-flash` (plain `flash` also rewrites the 12 MB WAD partition).
- Python for tools: `tools\venv` (Python 3.13, websockets 16, mcp 1.28).
- Working-tree files are CRLF (autocrlf); patches in `tools/` are CRLF too.

Conventions: the board accepts one `/agent` connection at a time; start a New Game
before connecting an agent; `sdkconfig` is local (gitignored), `sdkconfig.defaults` is
the tracked source of truth.

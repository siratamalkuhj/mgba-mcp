---
name: mgba-setup
description: "mgba-mcp setup: enabling the built-in MCP server in mGBA (GUI setting, --mcp flag, headless), connecting Claude Code, and troubleshooting connection problems. Use when setting up or debugging the mGBA MCP connection."
---

# mgba-mcp Setup

## Overview

mgba-mcp is a fork of mGBA with a **built-in MCP server**: no sidecar process. The emulator itself speaks MCP over Streamable HTTP at `http://127.0.0.1:2347/mcp`. Every tool runs on the emulation thread, so calls never race with the game or with each other.

Downloads (Windows): https://github.com/siratamalkuhj/mgba-mcp/releases/tag/windows-latest (`mgba-mcp-windows.zip` contains `mGBA.exe` and `mgba-headless.exe`).

## Enabling the server

### GUI (mGBA.exe) — recommended
1. Tools > Settings > General, check **Enable MCP server** (port defaults to 2347), OK.
2. Open a game. An on-screen message shows `MCP server: http://127.0.0.1:2347/mcp`.

The setting is remembered; every game opened afterwards starts the server. The game keeps running at normal speed until an agent pauses it.

### Command line
```
mGBA --mcp game.gba              # GUI, overrides the setting
mgba-headless --mcp game.gba     # no window
mgba-headless --mcp=2400 game.gba
```
On Windows, `start-mcp.bat` (drag a ROM onto it) starts the headless build.

**Headless differences:** no window (use `screenshot`), the game starts **paused**, and after `resume` it runs unthrottled (much faster than 60 fps). `load_rom` only works here.

## Connecting Claude Code

The plugin registers the server as `mgba`. Without the plugin:
```
claude mcp add --transport http mgba http://127.0.0.1:2347/mcp
```
Then `/mcp` should show `mgba` connected with 68 tools. The server must be running (game open) before Claude Code connects; if it was started later, choose Reconnect in `/mcp`.

## Transport details

- Streamable HTTP, `POST /mcp`, JSON responses (no SSE stream).
- MCP 2026-07-28 (stateless, `_meta` + `MCP-Protocol-Version`) and `initialize`-based 2024-11-05 … 2025-11-25.
- Binds to 127.0.0.1 only; requests with a non-local `Origin` get 403.
- Unknown tool → JSON-RPC error -32602. Tool failures come back as results with `isError: true` and a message saying what to change.

## Troubleshooting

1. **Connection refused:** no game is open, the setting is off, or the headless window was closed.
2. **`could not listen on port 2347`** on screen: another program holds the port (often a forgotten `mgba-headless` window). Close it or pick another port in settings and in the Claude Code registration.
3. **`/mcp` shows "Conflicting scopes"** or the wrong tool count: several servers are registered under the same name (e.g. an older Python-based `mgba` server). Keep this one, or register it under another name (`claude mcp add --transport http mgba-builtin ...`) and remove the stale entry with `claude mcp remove <name> -s <scope>`.
4. **`tools fetch failed` / invalid result:** an old build. Update to the latest release.
5. **`step` / `run_frames` refuse to run:** the GUI's own pause (Emulation > Pause) is active. Unpause in the GUI, then use the `pause` tool so MCP controls stepping.
6. **Tool calls take long:** `run_frames` in the GUI runs in real time (60 frames ≈ 1 s). Use fewer frames, or `set_speed fast_forward=true`.

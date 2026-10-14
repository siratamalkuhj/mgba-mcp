# mGBA Claude Code Plugin

Claude Code plugin for [mgba-mcp](https://github.com/siratamalkuhj/mgba-mcp), the mGBA (Game Boy Advance) fork with a built-in MCP server. Modeled on [duckstation-claude-plugin](https://github.com/sadnescity/duckstation-claude-plugin).

It connects Claude Code to the emulator's MCP server and adds skills that explain the 68 tools: playing games, debugging, memory inspection, GBA video/audio/hardware, cheats and value search, savestates and save data.

## Installation

```
/plugin marketplace add siratamalkuhj/mgba-mcp
/plugin install mgba@mgba-mcp
```

## Prerequisites

- mgba-mcp: [Windows download](https://github.com/siratamalkuhj/mgba-mcp/releases/tag/windows-latest), or build the `mcp` branch.
- In mGBA: Tools > Settings > General > **Enable MCP server**, then open a game. Or run `mgba-headless --mcp game.gba`.
- Default port 2347 (`http://127.0.0.1:2347/mcp`).

## Contents

- `.mcp.json`: connects to the server over Streamable HTTP.
- Skills:

| Skill | Covers |
| --- | --- |
| `mgba-setup` | Enabling the server, connecting, troubleshooting |
| `mgba-cpu-debug` | ARM/Thumb CPU, breakpoints (with conditions), watchpoints, stepping, traces, memory tools, GBA memory map |
| `mgba-video-audio-hw` | Display modes, BGs, tiles, palettes, sprites, sound, DMA, timers, interrupts |
| `mgba-system-io` | Playing with buttons and input sequences, speed, settings, savestates, save data, cheats, value search |
| `mgba-workflows` | Find RAM writers, find what draws a sprite, cheat search, RAM diffs, verifying ROM patches, text/pointer search, graphics extraction, playing turn by turn |

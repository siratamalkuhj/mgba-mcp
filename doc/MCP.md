# MCP server (mgba-mcp)

日本語版: [MCP_JP.md](MCP_JP.md)

This fork of mGBA adds a built-in [Model Context Protocol](https://modelcontextprotocol.io)
server, so AI agents such as Claude Code can play, inspect and debug GBA games running in mGBA.

The server lives inside the emulator (`src/debugger/mcp-server.c`). It is an ordinary debugger
module, so every request runs on the emulation thread and can safely touch emulator state.

## Download (Windows)

Automatic builds of the `mcp` branch are published at
https://github.com/siratamalkuhj/mgba-mcp/releases/tag/windows-latest
(`mgba-mcp-windows.zip`, containing both `mGBA.exe` and `mgba-headless.exe`).

## Starting it

**GUI (Qt):** Tools > Settings > General > check **Enable MCP server** (port 2347 by default),
then open a game. The server starts with every game until you turn it off. The game keeps running
at normal speed; agents can `pause` it and step frame by frame. `mGBA --mcp[=PORT] game.gba` also
works and overrides the setting.

**Headless:**

```sh
mgba-headless --mcp game.gba          # port 2347
mgba-headless --mcp=2400 game.gba     # custom port (note the '=')
```

The server only listens on `127.0.0.1` and rejects browser requests from non-local origins.

In the headless frontend the game starts **paused**, which is what you want for deterministic,
turn-based control. After `resume`, headless runs unthrottled (as fast as possible).

The SDL frontend parses `--mcp` too but has not been tested yet.

## Connecting Claude Code

Recommended: install the plugin (connection + five skills that teach Claude the tools):

```
/plugin marketplace add siratamalkuhj/mgba-mcp
/plugin install mgba@mgba-mcp
```

(The marketplace is read from the repository's default branch, which must be `mcp`.) Or register the server only:

```sh
claude mcp add --transport http mgba http://127.0.0.1:2347/mcp
```

Transport: Streamable HTTP, `POST /mcp`, JSON responses. Protocol revisions 2024-11-05 through
2025-11-25 (via `initialize`) and the stateless 2026-07-28 revision are accepted.

## Tools (68)

The feature set of [duckstation-claude-plugin](https://github.com/sadnescity/duckstation-claude-plugin) mapped onto GBA hardware. PS1-only parts (CD-ROM, GTE, PGXP, MDEC, dual memory cards) have no GBA equivalent; RetroAchievements, shaders and hotkeys are frontend features outside the core and are not exposed.

| Area | Tools |
| --- | --- |
| Play & status | `get_status`, `pause`, `resume`, `run_frames`, `press_buttons`, `set_buttons`, `input_sequence`, `get_controller_state`, `screenshot`, `reset`, `shutdown`, `set_speed`, `get_settings`, `set_setting` |
| Debugging | `step`, `step_over`, `step_out`, `trace`, `backtrace`, `get_registers`, `set_register`, `get_cpu_state`, `disassemble`, `add_breakpoint` (with conditions), `add_watchpoint`, `list_breakpoints`, `set_breakpoint_enabled`, `remove_breakpoint`, `clear_breakpoints` |
| Memory | `read_memory`, `write_memory` (inline, base64 or file), `list_memory_blocks`, `dump_memory`, `search_bytes`, `snapshot_memory`, `diff_memory`, `find_free_ram`, `memory_watch` |
| Video / audio / hardware | `get_video_state`, `dump_palette`, `dump_tiles`, `dump_background`, `list_sprites`, `dump_sprite`, `get_audio_state`, `get_hardware_state` |
| Saves & ROM | `save_state`, `load_state`, `undo_load_state`, `list_save_states`, `get_save_state_info`, `delete_save_state`, `get_rom_info`, `export_save`, `import_save`, `list_roms`, `load_rom` |
| Cheats | `add_cheat`, `list_cheats`, `set_cheat_enabled`, `remove_cheat`, `validate_cheat`, `apply_cheat_once`, `import_cheats`, `export_cheats`, `memory_search_start`, `memory_search_refine`, `memory_search_results` |

Every tool's description and input schema is available through `tools/list`; the plugin's skills explain how to combine them.

Addresses can be JSON numbers or strings such as `"0x02000000"`. Breakpoint conditions use mGBA's expression syntax with spaces between tokens (`r0 == 5`, `*0x03001000 > 3`; `*` reads a byte; decimal unless `0x`).

Cheats use mGBA's own cheat engine, so they are applied every frame and, in the GUI, saved per game
and shown in Tools > Cheats (reopen that window to see changes made over MCP). Value searches cover
work RAM by default (`region: "all"` includes every writable region). Note that mGBA's Game Boy VBA
code parser misreads values, so Game Boy freezes are generated as GameShark codes instead.

## How requests are scheduled

Requests are picked up at the end of every frame while the game runs, from the debugger's paused
loop, and while the frontend has the emulator paused. Tools that must not run mid-frame are
handled automatically:

- `step` and `run_frames` pause emulation first and stay paused.
- `reset` and `load_state` pause, run, then resume if the game was running.
- `step` and `run_frames` refuse to run while the *frontend* (not MCP) has paused emulation.

## Building

MCP support is on by default (`-DENABLE_MCP_SERVER=ON`) whenever debuggers are enabled. It vendors
[cJSON](https://github.com/DaveGamble/cJSON) (MIT) in `src/third-party/cjson`.

```sh
cmake -B build -DBUILD_HEADLESS=ON
cmake --build build
```

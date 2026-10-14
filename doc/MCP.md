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

```sh
claude mcp add --transport http mgba http://127.0.0.1:2347/mcp
```

Transport: Streamable HTTP, `POST /mcp`, JSON responses. Protocol revisions 2024-11-05 through
2025-11-25 (via `initialize`) and the stateless 2026-07-28 revision are accepted.

## Tools

| Tool | What it does |
| --- | --- |
| `get_status` | Running/paused, frame, game title/code, PC, CPU mode, held buttons, last stop reason |
| `pause` / `resume` | Pause at the end of the current frame / resume free-running |
| `step` | Execute N instructions (pauses first if running) |
| `run_frames` | Advance N frames, optionally holding buttons; `screenshot=true` returns the last frame |
| `press_buttons` | Press buttons for N frames then release (advances frames when paused) |
| `set_buttons` | Set exactly which buttons are held until changed |
| `screenshot` | Current screen as PNG |
| `reset` | Reset the console |
| `read_memory` | Hex dump or u8/u16/u32 values, side-effect free |
| `write_memory` | Write hex bytes or a 1/2/4-byte value |
| `list_memory_blocks` | Memory map |
| `get_registers` / `set_register` | CPU registers (plus mode and NZCV flags on GBA) |
| `disassemble` | ARM/Thumb disassembly around the PC or an address |
| `add_breakpoint` | Stop before executing an address |
| `add_watchpoint` | Stop on read/write/change of a memory range |
| `remove_breakpoint` / `list_breakpoints` | Manage breakpoints and watchpoints by id |
| `save_state` / `load_state` | Savestates by slot (1-9) or file path |
| `add_cheat` | Add a cheat: GameShark / Action Replay / CodeBreaker / VBA codes, or an address + value to freeze |
| `list_cheats` | List cheats, including ones added in the GUI |
| `set_cheat_enabled` / `remove_cheat` | Toggle or delete a cheat by index |
| `memory_search_start` | Start a value ("cheat") search: by known value, or `unknown` to snapshot RAM |
| `memory_search_refine` | Narrow it: equal / greater / less / changed / unchanged / increased / decreased / changed_by |
| `memory_search_results` | Current candidates with their current values |

Addresses can be JSON numbers or strings such as `"0x02000000"`.

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

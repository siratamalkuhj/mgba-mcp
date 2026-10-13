# MCP server (mgba-mcp)

日本語版: [MCP_JP.md](MCP_JP.md)

This fork of mGBA adds a built-in [Model Context Protocol](https://modelcontextprotocol.io)
server, so AI agents such as Claude Code can play, inspect and debug GBA games running in mGBA.

The server lives inside the emulator (`src/debugger/mcp-server.c`). It is an ordinary debugger
module, so every request runs on the emulation thread and can safely touch emulator state.

## Starting it

```sh
mgba-headless --mcp game.gba          # port 2347
mgba-headless --mcp=2400 game.gba     # custom port (note the '=')
```

The server only listens on `127.0.0.1` and rejects browser requests from non-local origins.

In the headless frontend the game starts **paused**, which is what you want for deterministic,
turn-based control. After `resume`, headless runs unthrottled (as fast as possible).

Qt frontend integration (a settings toggle) is planned. For now the `--mcp` flag works in the
headless frontend; the SDL frontend parses it too but has not been tested yet.

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

Addresses can be JSON numbers or strings such as `"0x02000000"`.

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

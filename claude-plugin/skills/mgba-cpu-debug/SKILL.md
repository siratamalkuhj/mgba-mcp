---
name: mgba-cpu-debug
description: "mGBA CPU and memory debugging over MCP: ARM7TDMI registers and modes, ARM/Thumb disassembly, breakpoints (with conditions), watchpoints, stepping (step, step_over, step_out), backtraces, instruction traces, memory read/write/dump/search, RAM snapshots and diffs, free RAM, memory watches. Use when debugging or reverse engineering GBA code."
---

# mGBA CPU & Memory Debugging

## GBA CPU essentials

- ARM7TDMI, 16.78 MHz. Two instruction sets: **ARM** (4-byte) and **Thumb** (2-byte; `BL` is a 4-byte pair). Most game code is Thumb, executed from ROM or IWRAM.
- `r13`=sp, `r14`=lr (return address; bit 0 set means return to Thumb), `r15`=pc. The raw `pc` register is 2 instructions ahead of the executing one; tools report the **current instruction** as `pc` in `get_status`/`step` and `currentInstruction` in `get_registers`.
- Modes: usr/sys (games), irq (interrupt handler), svc (BIOS calls via `swi`). Each mode has banked sp/lr/spsr: see `get_cpu_state`.

## Memory map

| Address | Region | Notes |
| --- | --- | --- |
| 0x00000000 | BIOS (16 KiB) | Only readable while executing BIOS |
| 0x02000000 | EWRAM (256 KiB) | Slow work RAM, big buffers |
| 0x03000000 | IWRAM (32 KiB) | Fast RAM: stack (~0x03007F00), hot code |
| 0x04000000 | I/O registers | Display, sound, DMA, timers, keys, IRQ |
| 0x05000000 | Palette (1 KiB) | BG 0x05000000, OBJ 0x05000200 |
| 0x06000000 | VRAM (96 KiB) | Tiles, maps, bitmaps; OBJ tiles at 0x06010000 |
| 0x07000000 | OAM (1 KiB) | 128 sprites × 8 bytes |
| 0x08000000 | ROM (up to 32 MiB) | Mirrored at 0x0A000000, 0x0C000000 |
| 0x0E000000 | Save RAM | SRAM/Flash (EEPROM is serial) |

`list_memory_blocks` gives exact ranges. Addresses can be numbers or strings like `"0x02000000"`.

## Execution control

- `pause` / `resume`, `get_status` (state, frame, pc, lastStop reason).
- `step count=N`: N instructions. `step_over`: runs a `BL`/`BLX` call to its return. `step_out`: runs to the caller.
  - `step_out` is exact when stack tracing is on: call `backtrace enable=true` **before** the function of interest is entered. Otherwise it runs until pc == lr, which is right for leaf functions but wrong once the function has called something else.
- `run_frames frames=N` advances whole frames (60 ≈ 1 s) and stops early on a breakpoint.
- `trace count=N` executes N instructions, logging each with all registers (inline ≤ 2000, `path` for up to 5,000,000).

## Breakpoints and watchpoints

- `add_breakpoint address=0x08001234` (Thumb bit is ignored). Optional `condition`, e.g. `"r0 == 5"`, `"r1 > 0x100"`, `"*0x03001000 == 3"`. **Separate tokens with spaces**; `*addr` reads one byte; numbers are decimal unless `0x`-prefixed.
- `add_watchpoint address=... length=N type=write|read|rw|change`. `change` only triggers when a write changes the value. When it hits, `get_status.lastStop` has the access type, old and new value, and `pc` is just after the accessing instruction (look one instruction back with `disassemble`).
- `list_breakpoints`, `set_breakpoint_enabled id=… enabled=false`, `remove_breakpoint id=…`, `clear_breakpoints`.
- After a hit: `get_registers`, `disassemble` (`=>` marks the next instruction), `backtrace`.

## Registers

- `get_registers`: all GPRs, cpsr, current instruction, mode, NZCV.
- `get_cpu_state`: decoded CPSR/SPSR, banked registers of every mode, halt state, pending interrupts.
- `set_register name=r0 value=0x10`.

## Memory

- `read_memory address length format=dump|u8|u16|u32` (≤ 4096 bytes), `format=base64` or `path=...` for up to 4 MiB. Reads have no side effects.
- `write_memory address data="DE AD"` / `value + width` / `base64` / `path`. Writes to I/O registers go through the bus, so they take effect like a game write. Writing to ROM addresses patches the loaded ROM in memory (not the file).
- `dump_memory region=wram|iwram|vram|oam|palette|rom|sram|io|bios [path]` saves a whole region.
- `search_bytes pattern="4D 41 ?? 49" region=ram|rom|all|<block>`: byte patterns with `??` wildcards (text, pointers, code).
- `snapshot_memory` then `diff_memory [width filter=changed|increased|decreased]`: what changed after an action.
- `memory_search_start` / `memory_search_refine` / `memory_search_results`: iterative value search (see mgba-system-io for cheats).
- `find_free_ram [fill min_size]`: largest runs of a fill byte in work RAM (candidate space for hooks; confirm with a write watchpoint).
- `memory_watch action=add address=… width=… label=…`, then `memory_watch` to see values and what changed since the last look.

## Tips

- Pointers into ROM look like `0x08xxxxxx`; little-endian, so `search_bytes "34 12 00 08"` finds pointers to 0x08001234.
- A write watchpoint on a value in IWRAM/EWRAM is the fastest way to find the code that owns it.
- `swi` calls (BIOS functions such as `CpuSet`, `LZ77UnComp`) appear as `swi 0x0B` etc. in Thumb.

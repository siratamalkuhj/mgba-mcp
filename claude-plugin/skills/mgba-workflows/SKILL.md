---
name: mgba-workflows
description: "GBA reverse engineering and automation workflows with mGBA MCP: find the code that writes a RAM value, find what draws a sprite or tile, cheat search and freezing, before/after RAM diffs, verifying ROM patches, finding text and pointers for translation, extracting graphics, automating menus, and playing a game turn by turn. Use when planning or carrying out a multi-step GBA debugging, ROM hacking or play task."
---

# GBA Workflows with mGBA MCP

## A. Find the code that writes a RAM value

1. Know the address (from workflow C or D).
2. `add_watchpoint address=<addr> length=<1|2|4> type=change`
3. `resume`, trigger the change in game (or `run_frames` while paused).
4. When `get_status.lastStop.reason == "watchpoint"`: it shows old/new value; `disassemble address=<pc - 4> count=8` shows the writing instruction (pc is just past it).
5. `backtrace` (if stack tracing was enabled earlier with `backtrace enable=true`) or `get_registers` lr gives the caller.
6. `remove_breakpoint id=<id>`.

Tip: if the write happens in a BIOS copy (`CpuSet`, DMA), look at lr / the DMA source to find what was copied from where.

## B. Find what draws a sprite or tile

1. `pause` with the element on screen. `list_sprites on_screen_only=true` and `dump_sprite index=<n> scale=4` to identify the sprite, or `get_video_state` + `dump_background` for a BG.
2. Sprite: its OAM entry is at `0x07000000 + 8 × index`; its tiles at `0x06010000 + 32 × tile`. BG: map entry at screen base + 2 × (y × 32 + x); tiles at char base + 32 × tile (4bpp).
3. `add_watchpoint` on the OAM entry (who positions it) or on its tiles (who uploads the graphics), `type=write`, then `resume`.
4. Many games build OAM in RAM and copy it with DMA at vblank: if the watchpoint stops in a DMA/CpuSet, put a second watchpoint on the RAM source buffer (`get_hardware_state subsystem=dma` shows the source).

## C. Cheat search and freeze

1. `memory_search_start value=<on-screen value> width=2`
2. Change the value in game, `memory_search_refine condition=equal value=<new>`; repeat.
3. Unknown encoding: start `condition=unknown`, refine `decreased` / `increased` / `unchanged`.
4. Verify: `write_memory address=<candidate> value=<test> width=2`, `screenshot`.
5. `add_cheat address=<addr> value=<desired> width=2 name="Max money"`.
6. To see who writes it, continue with workflow A.

## D. What does an action change? (RAM diff)

1. `pause` just before the action, `snapshot_memory`.
2. Do the action (`input_sequence`, or play with `press_buttons`).
3. `diff_memory width=1 filter=changed` (or `increased` / `decreased`, wider widths for counters).
4. Repeat with `update_snapshot=true` to separate noise (timers, RNG) from the values that follow the action.

## E. Verify a ROM patch (assembly hook)

1. `save_state slot=1` at a reproducible point.
2. Write the patch into ROM memory: `write_memory address=0x08xxxxxx data="..."` (Thumb `BL` to free space found with `find_free_ram` or in unused ROM).
3. `add_breakpoint address=<hook>`, `load_state slot=1`, `resume` or `run_frames`.
4. On the hit, `step` / `trace count=50` and check registers and memory.
5. Iterate with `load_state slot=1`; `undo_load_state` if you load the wrong slot. The patch only lives in memory until the ROM is reloaded.

## F. Find text and pointers (translation)

1. Find a string from the screen: if it's ASCII, `search_bytes pattern="<hex of text>" region=rom`. Many games use custom encodings: find a known word's relative pattern, or watch the RAM text buffer (`snapshot_memory` before the dialog, `diff_memory` after) and then a watchpoint on the buffer to reach the decoder.
2. Pointers to a string at 0x08ABCDEF: `search_bytes pattern="EF CD AB 08" region=rom`.
3. Font graphics: `dump_tiles char_block=<n>` while text is displayed; `get_video_state` shows which block the text layer uses.

## G. Extract graphics

`dump_background layer=<n> path=...`, `dump_tiles char_block=<n> palette=<p> path=...`, `dump_sprite index=<n> scale=1 path=...`, `dump_palette include_values=true`. Raw data: `dump_memory region=vram|palette|oam path=...`. Compressed source graphics in ROM are usually LZ77 (`swi 0x11/0x12`): a breakpoint on the decompression call shows the ROM source address in r0.

## H. Automate menus and play turn by turn

- Navigate: `input_sequence steps=[...] screenshot=true` while paused, inspect, adjust.
- Play: `pause`, then loop `press_buttons ... screenshot=true` (or `run_frames frames=30 buttons=[...] screenshot=true` for movement), deciding from each screenshot. Read RAM values (HP, position) with `memory_watch` once found via workflow C — far more reliable than reading pixels.
- Before risky choices `save_state`, and roll back with `load_state` / `undo_load_state`.
- Long waits (cutscenes): `set_speed fast_forward=true` in the GUI, or `run_frames frames=600`.

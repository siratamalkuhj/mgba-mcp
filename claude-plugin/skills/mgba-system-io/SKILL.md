---
name: mgba-system-io
description: "Playing and controlling games through mGBA MCP: buttons, frame stepping, input sequences, screenshots, speed, settings, savestates and undo, cartridge save data, ROM info, loading ROMs, cheats (codes and frozen values) and value search. Use when playing a GBA game with tools, automating input, managing saves, or creating cheats."
---

# mGBA System Control, Input, Saves & Cheats

## Playing

GBA buttons: `A`, `B`, `Select`, `Start`, `Right`, `Left`, `Up`, `Down`, `R`, `L`.

Deterministic turn-based loop (recommended):
1. `pause`
2. `press_buttons buttons=["A"] frames=6 screenshot=true` — presses, advances, releases, returns the frame
3. Look at the screenshot, decide, repeat.

- `run_frames frames=N [buttons] [screenshot]`: hold buttons for N frames (60 ≈ 1 s).
- `input_sequence steps=[{buttons:["Down"],frames:8},{buttons:[],frames:4},{buttons:["A"],frames:2}] [screenshot]`: menu navigation in one call. While paused it runs immediately; while running it plays in the background.
- `set_buttons buttons=[...]`: hold until changed (`[]` releases all). `get_controller_state` shows what the emulator holds and what the game sees.
- Menus usually need a press of ~2–6 frames followed by a release; holding a direction repeats it.
- `screenshot [path]`, `get_status`.
- `set_speed speed=2` or `fast_forward=true` (GUI only; headless is always unthrottled).

## Settings

`get_settings [key]`, `set_setting key value` (session only), e.g. `mute=true`, `frameskip=1`, `volume=128`.

## Savestates

- `save_state slot=1..9` (or `path`), `load_state slot|path`.
- `undo_load_state`: return to just before the last load (use freely when experimenting).
- `list_save_states`: slots with save time; `get_save_state_info slot` returns the time and the screenshot stored in the state.
- `delete_save_state slot`.
Slots are shared with the GUI's File > Save/Load State menus.

## Cartridge save data

- `get_rom_info`: title, game code, maker, version, CRC32, ROM size, save type/size, cartridge hardware (RTC, gyro, tilt, rumble…), BIOS.
- `export_save [path]`: write the battery save (.sav) to a file.
- `import_save path`: replace it (the old save is backed up to the temp folder; the path is returned). Most games read saves at boot: `reset` after importing.

## Game control

- `reset`, `shutdown confirm=true` (closes the game; exits headless).
- `list_roms directory`, `load_rom path` (headless only; in the GUI use File > Load ROM).

## Cheats

mGBA's cheat engine applies enabled cheats every frame; cheats are saved per game and shown in the GUI (Tools > Cheats; reopen that window to see MCP changes).
- `add_cheat code="..." [type]`: GameShark, Action Replay, CodeBreaker or VBA codes, auto-detected (`type=gameshark|action_replay|codebreaker|vba`, GB: `game_genie`). Several codes may be separated by spaces or newlines.
- `add_cheat address value width=1|2|4 [name]`: freeze a value (infinite money, HP…).
- `list_cheats`, `set_cheat_enabled index enabled`, `remove_cheat index` (later indexes shift).
- `validate_cheat code`: check parsing without adding. `apply_cheat_once code|index`: write once.
- `import_cheats text|path` (mGBA .cheats format: `# Name` then codes, `!disabled` before a name to add it disabled), `export_cheats [path]`.

### Finding a value to freeze (value search)

1. `memory_search_start value=<shown value> width=1|2|4` (2 or 4 if it can exceed 255).
2. Change it in game (spend money, take damage), then `memory_search_refine condition=equal value=<new value>`.
3. Repeat until a few candidates remain (`memory_search_results`).
4. Confirm with `write_memory` on a candidate and a `screenshot`.
5. `add_cheat address=<it> value=<wanted> width=<same>`.

If the number on screen isn't stored as-is (BCD, ×10, offset), start with `condition=unknown` and refine with `increased` / `decreased` / `unchanged` / `changed_by value=-N`. Searches cover work RAM unless `region=all`.

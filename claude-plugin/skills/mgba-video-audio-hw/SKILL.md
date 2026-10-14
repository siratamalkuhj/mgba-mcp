---
name: mgba-video-audio-hw
description: "GBA video, audio and hardware over mGBA MCP: display modes, background layers, tiles, tile maps, palettes, sprites/OAM, windows and blending, sound channels, DMA, timers, interrupts, keypad and wait states. Use when inspecting or extracting GBA graphics, or debugging rendering, sound or hardware behavior."
---

# GBA Video, Audio & Hardware

## Display primer

- 240×160, 15-bit color (BGR555: `0bbbbbgggggrrrrr`). Registers at 0x04000000.
- **DISPCNT** selects the mode:
  - Mode 0: BG0–3 tiled ("text") layers. Mode 1: BG0–1 text + BG2 affine. Mode 2: BG2–3 affine.
  - Modes 3/4/5: BG2 is a bitmap (3: 240×160 direct color; 4: 240×160 paletted, 2 frames; 5: 160×128 direct, 2 frames).
- A text BG has a **char base** (tiles, 16 KiB blocks at 0x06000000 + n×0x4000) and a **screen base** (map, 2 KiB blocks at 0x06000000 + n×0x800). Map entries: tile (10 bits), h/v flip, palette (4 bits). Tiles are 8×8, 4bpp (32 bytes, 16 palettes of 16) or 8bpp (64 bytes, one 256-color palette).
- Sprites (OBJ): 128 entries in OAM, tiles from 0x06010000, palettes at 0x05000200. DISPCNT bit 6 selects 1D or 2D tile mapping.
- Color 0 of every palette row is transparent; BG palette color 0 is the backdrop.

## Tools

- `get_video_state`: decoded DISPCNT (mode, layers, OBJ mapping, forced blank), each BG (kind, priority, char/screen base, bpp, size, scroll, affine matrix), windows, blending (mode, EVA/EVB/EVY), VCOUNT.
- `dump_palette [include_values]`: 16×32 swatch image (rows 0–15 BG palettes, 16–31 OBJ palettes).
- `dump_tiles char_block=0..5 bpp=4|8 palette=n [palette_type]`: a 16 KiB tile block as an image (blocks 4–5 are sprite tiles).
- `dump_background layer=0..3`: the whole layer as stored (full map for text/affine, frame buffer for bitmap modes; `frame` picks the page in modes 4/5). The visible screen is a 240×160 window at the scroll position.
- `list_sprites [on_screen_only include_hidden]`: decoded OAM. Note an all-zero OAM entry is a visible 8×8 sprite at (0,0); games hide sprites with the disable bit.
- `dump_sprite index scale`: one sprite as an image (unrotated; flip applied).
- `screenshot [path]`: the composed frame. All image tools accept `path` to also save a PNG.

Find what draws something: `add_watchpoint` on its VRAM tiles, map entries or OAM entry (`type=write`), resume, and read the pc. Many games copy graphics with DMA or `swi CpuSet/LZ77UnComp`; the watchpoint then stops inside that copy, and `backtrace`/lr leads to the caller.

## Audio

`get_audio_state`: master enable, which PSG channels are playing (square 1/2, wave, noise), PSG mixing, DirectSound A/B volume, L/R routing and timer, and per-channel settings with frequencies. Most music uses DirectSound (PCM via timer 0/1 + DMA 1/2), so check `get_hardware_state subsystem=dma` and `timers` too.

## Hardware

`get_hardware_state subsystem=dma|timers|interrupts|keypad|system|all`:
- **DMA 0–3**: enabled, source, dest, count, timing (immediate/vblank/hblank/special), address control, repeat, unit size, IRQ.
- **Timers 0–3**: counter, reload, prescaler, count-up, IRQ.
- **Interrupts**: IME, enabled/requested/pending sources (vblank, hblank, vcounter, timers, serial, DMA, keypad, gamepak), CPU halted.
- **Keypad**: buttons the game sees in KEYINPUT, key-IRQ settings.
- **System**: WAITCNT wait states, prefetch, real vs HLE BIOS.

Hardware registers can be written with `write_memory` (e.g. `address=0x04000000 value=0x0100 width=2` to show only BG0), which goes through the bus like a game write.

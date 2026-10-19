# Porting the game logic

The game logic is the C of [3s-decomp-ogxb](https://github.com/Manya3084/3s-decomp-ogxb), compiled from a checkout of it (`SF_DECOMP_DIR`, default `../3s-decomp-ogxb`). The checkout is read, never written. `cmake/decomp.cmake` builds it into `libsf_decomp.a`.

The port needs the decomp's `xbox-port-data` branch until it is merged: `color_file` with the Xbox file numbers, the shared structs in place of the files' 32-bit partial copies (`tools/unify_structs.py`), and names for the retail addresses the code used as numbers.

No Xbox code is built. Out of the build:

- `src/sf33rd/Source/XBOX/`: the platform layer (`fl*` texture and palette handles, render states, pads, print, frame end), the Xbox-only game code (the `xb_effect_*_draw` functions, the save menu, the quad renderers, the exit and screenshot screens), and the Xbox Live code.
- `src/cri/`: CRI's Xbox middleware. `src/game/adx.c` and `aix.c` decode natively.
- `Compress/Lz77` and the decomp's zlib copy. The disc uses zlib inflate; the system zlib is linked.
- The XBE. Only its XACT sound banks are read, at import (`src/iso/xbe.c`).

What the game logic calls in those files is written here, in `src/port/`, on SDL3, the Vulkan and OpenGL renderer in `src/render/`, and OpenAL. The Xbox C is the reference for what each call does. It is not compiled.

## Stubs

`tools/gen_stubs.py` runs at link time. Every name the game logic needs that neither `src/port/` nor the host libraries define gets a stub in `build/decomp/stubs.c`:

- a function reports its first call (`port: no shim for NAME`) and returns 0;
- data, meaning any name used other than by a call, is a zeroed 64 KiB block;
- a folded name from the decomp's `tools/fold_aliases.py` jumps to the body it was merged into.

The stub list is the native work left. Defining a name in `src/port/` removes its stub on the next build. `SF_PORT_STRICT=1` aborts on the first stubbed call.

## x86-64

The game code keeps some addresses in `u32` words. `sf33rdsxb_game` is linked non-PIE, so code and globals sit below 2 GB, and `sf_low_alloc` (`src/port/lowmem.c`) gives the game heap memory below 2 GB with `MAP_32BIT`. The decomp's `include/` is copied into the build tree with `size_t` and the pointer-sized integers widened. A decomp file that does not compile for x86-64 is listed in `cmake/decomp-overlay.txt` and compiled from a changed copy: `lowaddr` runs `tools/lowaddr_init.py`, which moves address words out of static initializers into a constructor, and `patch` applies a patch from `cmake/decomp-overlay/`. Each such change belongs upstream once the decomp can take it without changing its relink.

The decomp was written to match the 32-bit build byte for byte, and its 64-bit faults come in three forms:

- a file's own partial copy of a shared struct, padded for the 32-bit layout;
- an address kept in a `u32` table;
- a global declared with one type in one file and another type elsewhere. Both types have the same size on 32-bit and different sizes on x86-64. One case was the effect pool: 69 files indexed it in rows of 0x700 bytes, while it holds 448 pointer-sized words a row.

These are fixed in the decomp itself, and each fix must leave the i386 code and data unchanged. `tools/unify_structs.py` (in the decomp) replaces partial copies with `structs.h`. `tools/lto_type_check.sh BUILD_DIR` lists every global whose declarations disagree. The warnings it still prints are prototypes that differ only in small integer widths, and array bounds.

## What src/port holds

- `gfx.c`: texture and palette handles, the render state call, the quads, the PS2-sized picture's copy. `render/scene.c` draws them on Vulkan at a whole multiple of 640x480 (the window's, or `SF_SCALE`), shown 4:3. Each quad's edges move to the 640x480 pixels D3D would cover, with its texture coordinates recomputed at the moved corners, and each sample is taken at its 640x480 pixel's centre within the quad's texture rectangle: a larger screen shows exactly the pixels 640x480 does, without the seams a finer grid opens between the game's tiles.
- `system.c`: the start-up (`njUserInit`), the task loop, the work pool.
- `files.c`: CRI's file calls over `data/afs/SF33RDXB.AFS`.
- `input.c`: the keyboard (port 0) and SDL3 gamepads (ports 1 to 3) through retail's pad conversion and `keyConvert`.
- `save.c`: the saves, as files in `data/save`.
- `sound.c`: CRI's ADXT and AIXP players on OpenAL streaming sources, fed by `src/game/adx.c` and `aix.c`.
- `se.c`: the sound effects. The game's sound engine (CapSndEng's `cseTsbRequest`) queues requests for retail's XACT layer. `se.c` is that layer: the request queue, 48 voices, group and priority stops, and the pan tables. It plays XACT cues from the sound banks in `data/audio/sdbk`, whose layout is the XDK's `_XACT_SOUNDBANK_*` types, over the wave banks the game loads into memory (`src/game/xwb.c` decodes PCM and Xbox ADPCM). Voices end by the frame count, so effects do not change a replay. `SF_SE_DEBUG=1` logs every effect that starts.
- `frame.c`, `video.c`: one frame in retail's order; the window, the headless renderer, the 60 fps pacing and the captures.
- `live.c`: the Xbox Live calls the offline game still makes, answered as offline.
- `zlib.c`, `math.c`, `banks.c`: the game's zlib calls, its D3DX matrix helpers, the sound bank buffers.

The memory manager is `src/game/memman.c`: the decomp's keeps two pointers in a cell header that must fit the 16-byte unit.

## Debugging

`SF_SHOT=FRAME:PATH` writes the output after that frame as a PPM (`SF_SHOT_SCREEN=1` the PS2-sized picture before its copy, `2` the layer drawn over it). `SF_DUMP_QUADS=FRAME` lists the frame's quads, `SF_DUMP_TEX=FRAME` writes its textures to the working directory. `SF_AUTOPRESS=F[:BUTTON],...` holds a button (Start by default) for six frames from each frame F. `SF_HEADLESS=1` runs with no window (Vulkan offscreen; with `SF_SHOT` for captures), `SF_NO_GAMEPADS=1` ignores real pads. Every session's inputs go to `data/last_session.input` (`SF_RECORD=PATH` elsewhere); `SF_REPLAY=PATH` plays a recording back, and the game follows it frame for frame; after its end the game reads the live input again. Headless runs are not paced. A crash writes its backtrace to `data/crash.log`. `SF_INPUT_DEBUG=1` prints pad changes. `SF_COUNTRY=1..4` picks the region (4, North America, by default).

## Frame

`src/port/frame.c` is one frame in the order retail's frame function runs it, without the Live calls. `sf_game_init` is its first-frame branch.

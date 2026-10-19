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

The game code keeps some addresses in `u32` words. `sf33rdsxb_game` is linked non-PIE, so code and globals sit below 2 GB, and `sf_low_alloc` (`src/port/lowmem.c`) gives the game heap memory below 2 GB with `MAP_32BIT`. The decomp's `include/` is copied into the build tree with `size_t` and the pointer-sized integers widened. A decomp file that does not compile for x86-64 is listed in `cmake/decomp-overlay.txt` and compiled from a changed copy: `lowaddr` runs `tools/lowaddr_init.py`, which moves address words out of static initializers into a constructor, and `patch` applies a patch from `cmake/decomp-overlay/`. Each such change belongs upstream once the decomp can take it without changing its relink, except the port's own features, listed last in the file: SCREEN ADJUST's three window rows patch `menu.c` (ten rows), `EFF61.c` (their labels), `EFF63.c` (their values come from `display.c`) and `Sel_Data.c` (the rows closer together). Those stay here.

The decomp was written to match the 32-bit build byte for byte, and its 64-bit faults come in three forms:

- a file's own partial copy of a shared struct, padded for the 32-bit layout;
- an address kept in a `u32` table;
- a global declared with one type in one file and another type elsewhere. Both types have the same size on 32-bit and different sizes on x86-64. One case was the effect pool: 69 files indexed it in rows of 0x700 bytes, while it holds 448 pointer-sized words a row.

These are fixed in the decomp itself, and each fix must leave the i386 code and data unchanged. `tools/unify_structs.py` (in the decomp) replaces partial copies with `structs.h`. `tools/lto_type_check.sh BUILD_DIR` lists every global whose declarations disagree. The warnings it still prints are prototypes that differ only in small integer widths, and array bounds.

## What src/port holds

- `gfx.c`: texture and palette handles, the render state call, the quads, the PS2-sized picture's copy. `render/scene.c` draws them on Vulkan at a whole multiple of 640x480 (the window's, or `SF_SCALE`), shown 4:3. Each quad's edges move to the 640x480 pixels D3D would cover, with its texture coordinates recomputed at the moved corners, and each sample is taken at its 640x480 pixel's centre within the quad's texture rectangle: a larger screen shows exactly the pixels 640x480 does, without the seams a finer grid opens between the game's tiles.
- `render/glscene.c`: the same screen on OpenGL 3.3, where Vulkan is missing or `SF_RENDERER=opengl` asks for it: scene.c's passes and shaders (GLSL 330, the texture bound per batch), each framebuffer's rows top first so the numbers match Vulkan's, and the window draw turned over. It gives the same pixels as Vulkan. `SF_HEADLESS=1 SF_RENDERER=opengl` runs it on SDL's offscreen driver for tests.
- `system.c`: the start-up (`njUserInit`), the task loop, the work pool.
- `files.c`: CRI's file calls over `data/afs/SF33RDXB.AFS`.
- `input.c`: the keyboard (port 0) and SDL3 gamepads (ports 1 to 3) through retail's pad conversion and `keyConvert`.
- `save.c`: the saves, as files in `data/save`.
- `sound.c`: CRI's ADXT and AIXP players on OpenAL streaming sources, fed by `src/game/adx.c` and `aix.c`. Each frame takes 1/60 s of samples from each stream, so the statuses the game waits on (a stem ending) fall on the same frames with or without a device, and a replay stays exact. `SF_SOUND_DEBUG=1` logs the stream calls.
- `se.c`: the sound effects. The game's sound engine (CapSndEng's `cseTsbRequest`) queues requests for retail's XACT layer. `se.c` is that layer: the request queue, 48 voices, group and priority stops, and the pan tables. It plays XACT cues from the sound banks in `data/audio/sdbk`, whose layout is the XDK's `_XACT_SOUNDBANK_*` types, over the wave banks the game loads into memory (`src/game/xwb.c` decodes PCM and Xbox ADPCM). Voices end by the frame count, so effects do not change a replay. `SF_SE_DEBUG=1` logs every effect that starts, and `SF_SE_LFO=0` leaves out the pitch LFO's vibrato.
- `frame.c`, `video.c`: one frame in retail's order; the window, the headless renderer, the 60 fps pacing and the captures.
- `menu_ext.c`: what the port adds to the menus, drawn with the game's pieces (its tone-down, the training menu's red band, its menu font and menu sounds). These are EXIT GAME in place of SELECT GAME (its prompt, and `Reboot_Program` quitting the port) and BUTTON CONFIG's keyboard page (F1). While the page is open the game sees nothing pressed. Also `xb_ToneDownCopy`.
- `display.c`: the window mode, resolution and aspect ratio behind SCREEN ADJUST's rows, kept in `config/display.txt`.
- `live.c`: the Xbox Live calls the offline game still makes, answered as offline.
- `zlib.c`, `math.c`, `banks.c`: the game's zlib calls, its D3DX matrix helpers, the sound bank buffers.

The memory manager is `src/game/memman.c`: the decomp's keeps two pointers in a cell header that must fit the 16-byte unit.

## Game state and rollback

Rollback netcode runs frames again from an earlier one, so the game's state has to be saved and put back whole. `src/port/state.c` does that: the state is the program's writable data (the decomp's globals and the generated stubs' blocks) less the port's own (`sfport_data` and `sfport_bss`: the build renames the port libraries' `.data` and `.bss`, cmake/decomp.cmake), plus the used part of the low arena (the game's heaps), plus the music streams' place (`sound.c` saves which file each stream plays, where, and its stem queue, and decodes again on a restore). Port files that define the game's own globals (retail kept them in its platform files: `mpp_w`, `plw`, the work pool's ends in `system.c`, the sound settings in `save.c`) put them in the state with `SF_GAME`; `SF_KEEP` keeps the snapshot's own bookkeeping and the input tape out of it.

`SF_ROLLBACK_TEST=FRAME:K` saves at FRAME, runs K frames, puts the state back and runs them again on the same pads (`input.c`'s tape), comparing the state after each; it names the pages that differ. In a fight the frames come out the same (the snapshot is 43 MB, of which a frame changes about 250 KB, so a rollback layer would save changed pages only); across character select and the loads after it they do not yet, as files and textures are made through the port there. `SF_ROLLBACK_ALL=1` takes the port's state in too and `SF_ROLLBACK_EXTRA=ADDR:LEN,...` given ranges (the link map, `build/sf33rdsxb_game.map`, has each file's), for finding what else the game reads back.

## Debugging

`SF_SHOT=FRAME:PATH` writes the output after that frame as a PPM (`SF_SHOT_SCREEN=1` the PS2-sized picture before its copy, `2` the layer drawn over it). `SF_SHOT_EVERY=N:DIR` writes one every N frames (between `SF_SHOT_FROM` and `SF_SHOT_TO` when set). `SF_DUMP_QUADS=FRAME` lists the frame's quads (with each one's clamp rectangle), `SF_DUMP_TEX=FRAME` writes its textures to the working directory. `SF_AUTOPRESS=F[:BUTTON],...` holds a button (Start by default) for six frames from each frame F. `SF_HEADLESS=1` runs with no window (Vulkan offscreen; with `SF_SHOT` for captures), `SF_NO_GAMEPADS=1` ignores real pads. Every session's inputs go to `data/last_session.input` (`SF_RECORD=PATH` elsewhere); `SF_REPLAY=PATH` plays a recording back, and the game follows it frame for frame; after its end the game reads the live input again. Headless runs are not paced unless `SF_PACE=1` is set (for capturing audio with OpenAL Soft's wave writer). `SF_KEYPAGE=FRAME` opens the keyboard page on that frame. A crash writes its backtrace to `data/crash.log`. `SF_INPUT_DEBUG=1` prints pad changes. `SF_COUNTRY=1..4` picks the region (4, North America, by default).

## Frame

`src/port/frame.c` is one frame in the order retail's frame function runs it, without the Live calls. `sf_game_init` is its first-frame branch.

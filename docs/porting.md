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

Every address the game code keeps is pointer-sized, so `sf33rdsxb` is an ordinary PIE (on Windows, a relocatable image with high-entropy ASLR), and its memory can be anywhere: `sf_arena_alloc` (`src/port/arena.c`) takes the game's heaps from one block wherever the system puts it. The decomp's `include/` is copied into the build tree with `size_t` and the pointer-sized integers widened. A decomp file that does not compile for x86-64 is listed in `cmake/decomp-overlay.txt` and compiled from a changed copy, a patch from `cmake/decomp-overlay/`. Each such change belongs upstream once the decomp can take it without changing its relink, except the port's own features, listed last in the file: SCREEN ADJUST's three window rows patch `menu.c` (ten rows), `EFF61.c` (their labels), `EFF63.c` (their values come from `display.c`) and `Sel_Data.c` (the rows closer together). Those stay here.

Until October 2026 the program was linked non-PIE at a low address, with its heaps below 2 GB (`MAP_32BIT`), because the decomp kept some addresses in `u32` words: the background and ending rewrite tables, the character files' table offsets turned into addresses in place, a player's effect read as its 203rd word, a table of text pointers read as `s32`s, and the AIX player passed as an `s32`. Those are pointer-sized in the decomp now (`uintptr_t`, which is `u32` on the Xbox, so its i386 objects did not change). The check that found them was a replay of a long recorded session with the heap placed where a truncated address faults (`0x7e00c0000000`), then with the program as a PIE under ASLR; every compared frame matched the low-address build. `clang -Wshorten-64-to-32 -Wint-to-pointer-cast -Wpointer-to-int-cast` over the decomp lists what is left: the size truncations in `bg.c` (sizes, not addresses).

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
- `input.c`: the keyboard (port 0) and SDL3 gamepads (ports 1 to 3) through retail's pad conversion and `keyConvert`. Bindings are kept in `config/host.sqlite`.
- `save.c`: the saves, in `data/save/game.sqlite`. A missing database is created from the original defaults. A damaged database is left as it is. `store.c` is the SQLite file.
- `sound.c`: CRI's ADXT and AIXP players on OpenAL streaming sources, fed by `src/game/adx.c` and `aix.c`. Each frame takes 1/60 s of samples from each stream, so the statuses the game waits on (a stem ending) fall on the same frames with or without a device, and a replay stays exact. `SF_SOUND_DEBUG=1` logs the stream calls.
- `se.c`: the sound effects. The game's sound engine (CapSndEng's `cseTsbRequest`) queues requests for retail's XACT layer. `se.c` is that layer: the request queue, 48 voices, group and priority stops, and the pan tables. It plays XACT cues from the sound banks in `data/audio/sdbk`, whose layout is the XDK's `_XACT_SOUNDBANK_*` types, over the wave banks the game loads into memory (`src/game/xwb.c` decodes PCM and Xbox ADPCM). Voices end by the frame count, so effects do not change a replay. `SF_SE_DEBUG=1` logs every effect that starts, and `SF_SE_LFO=0` leaves out the pitch LFO's vibrato.
- `frame.c`, `video.c`: one frame in retail's order; the window, the headless renderer, the 60 fps pacing and the captures.
- `menu_ext.c`: what the port adds to the menus, drawn with the game's pieces (its tone-down, the training menu's red band, its menu font and menu sounds). These are EXIT GAME in place of SELECT GAME (its prompt, and `Reboot_Program` quitting the port) and BUTTON CONFIG's keyboard page (F1). While the page is open the game sees nothing pressed. Also `xb_ToneDownCopy`.
- `display.c`: the window mode, resolution and aspect ratio behind SCREEN ADJUST's rows, kept in `config/host.sqlite` and written when a row changes. FILTER is the game's own row: OFF, SMOOTH, CRT CURVED or CRT FLAT (`sys_w.screen_mode`), drawn when the picture is shown. CRT FLAT is the curved one with square corners. FILTER and the picture size are written to the system database when they change.
- `live.c`: the Xbox Live calls the offline game still makes, answered as offline.
- `zlib.c`, `math.c`, `banks.c`: the game's zlib calls, its D3DX matrix helpers, the sound bank buffers.

The memory manager is `src/game/memman.c`: the decomp's keeps two pointers in a cell header that must fit the 16-byte unit.

## Game state and rollback

Rollback netcode runs frames again from an earlier one, so the game's state has to be saved and put back whole. `src/port/state.c` does that: the state is the program's writable data (the decomp's globals and the generated stubs' blocks) less the port's own (`sfport_data` and `sfport_bss`: the build renames the port libraries' `.data` and `.bss`, cmake/decomp.cmake), plus the used part of the arena (the game's heaps), plus the music streams' place (`sound.c` saves which file each stream plays, where, and its stem queue, and decodes again on a restore). Port files that define the game's own globals (retail kept them in its platform files: `mpp_w`, `plw`, the work pool's ends in `system.c`, the sound settings in `save.c`) put them in the state with `SF_GAME`; `SF_KEEP` keeps the snapshot's own bookkeeping and the input tape out of it.

`SF_ROLLBACK_TEST=FRAME:K` saves at FRAME, runs K frames, puts the state back and runs them again on the same pads (`input.c`'s tape), comparing the state after each; it names the pages that differ. In a fight the frames come out the same (the snapshot is 43 MB, of which a frame changes about 250 KB, so a rollback layer would save changed pages only); across character select and the loads after it they do not yet, as files and textures are made through the port there. `SF_ROLLBACK_ALL=1` takes the port's state in too and `SF_ROLLBACK_EXTRA=ADDR:LEN,...` given ranges (the link map, `build/sf33rdsxb_game.map`, has each file's; the program's CMake target is still `sf33rdsxb_game`), for finding what else the game reads back.

## Threads

The game itself runs on one thread, at a locked 60 frames a second (its own clock, `pace()` in `src/port/video.c`), and everything that thread does each frame decides the game's state, which a replay and rollback need to be exact. What can leave it does, so a slower machine with more cores keeps the 60:

- **The render thread** (`src/port/video.c`). At the end of a frame the game thread waits for the render thread to finish the frame before (it has the rest of a frame to do it), copies the new draw list into the render thread's own (the vertices and batches, and only the textures and palettes whose version changed), and runs on; the render thread draws, presents and waits on the display's refresh. The frame goes to the GPU as soon as it is built. Every other renderer call runs on the render thread between frames, so the renderer is used from one thread (OpenGL's context is handed to it, `sf_render_make_current`). Paced at 60, the hand-off costs the game thread a few microseconds where drawing and presenting took about 80.
- **Workers** (`src/port/jobs.c`): one fewer than the cores, two at most. They read the disc and decode the sound. The game thread asks for a job's result where it needs it and waits only if it is not done, so a job never moves a frame:
  - the game's disc reads (`ADXF_ReadNw` in `src/port/files.c`) run from the request; the status poll, on the next frame, waits for the read;
  - a music stream from the archive reads only its header at its start; a worker reads and decodes the rest in 64 KB parts while it plays (`sf_adx_open`, `sf_adx_step` in `src/game/adx.c`), a queued stem from when it is queued, the opening theme once when the device opens. Up to a sample already decoded, the header's length and loop decide as the decoded ones do, so every status lands on the frame it would with a whole decode;
  - a sound-effect wave bank is decoded when it is registered, and a wave's first play takes it from there;
  - the first start's import, while the window keeps drawing.
- OpenAL Soft mixes and plays on its own thread.

`SF_THREADS=N` sets the workers (0 runs each job on the thread that asks for it, as before there were workers), `SF_RENDER_THREAD=0` draws on the game thread; a one-core machine does both. A replay of a recorded session gives the same frames with and without them.

## Debugging

`SF_SHOT=FRAME:PATH` writes the output after that frame as a PPM (`SF_SHOT_SCREEN=1` the PS2-sized picture before its copy, `2` the layer drawn over it). `SF_SHOT_EVERY=N:DIR` writes one every N frames (between `SF_SHOT_FROM` and `SF_SHOT_TO` when set). `SF_DUMP_QUADS=FRAME` lists the frame's quads (with each one's clamp rectangle), `SF_DUMP_TEX=FRAME` writes its textures to the working directory. `SF_AUTOPRESS=F[:BUTTON],...` holds a button (Start by default) for six frames from each frame F. `SF_HEADLESS=1` runs with no window (Vulkan offscreen; with `SF_SHOT` for captures), `SF_NO_GAMEPADS=1` ignores real pads. Every session's inputs go to `data/last_session.input` (`SF_RECORD=PATH` elsewhere); `SF_REPLAY=PATH` plays a recording back, and the game follows it frame for frame; after its end the game reads the live input again. Headless runs are not paced unless `SF_PACE=1` is set (for capturing audio with OpenAL Soft's wave writer). `SF_KEYPAGE=FRAME` opens the keyboard page on that frame. A crash writes its backtrace to `data/crash.log`. `SF_INPUT_DEBUG=1` prints pad changes. `SF_COUNTRY=1..4` picks the region (4, North America, by default). `SF_SETUP_SOURCE=PATH` gives the first start's screen that source as if it were dropped on it. `SF_TRACE=NAME:SIZE,...` prints those globals, by symbol name, after each frame (between `SF_TRACE_FROM` and `SF_TRACE_TO`): two builds run over the same recording print the same lines until they part, which finds where a compiler or a processor makes the game go otherwise.

## Frame

`src/port/frame.c` is one frame in the order retail's frame function runs it, without the Live calls. `sf_game_init` is its first-frame branch.

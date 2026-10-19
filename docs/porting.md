# Porting the game logic

The game logic is the C of [3s-decomp-ogxb](https://github.com/Manya3084/3s-decomp-ogxb), compiled from a checkout of it (`SF_DECOMP_DIR`, default `../3s-decomp-ogxb`). The checkout is read, never written. `cmake/decomp.cmake` builds it into `libsf_decomp.a`.

No Xbox code is built. Out of the build:

- `src/sf33rd/Source/XBOX/`: the platform layer (`fl*` texture and palette handles, render states, pads, print, frame end), the Xbox-only game code (the `xb_effect_*_draw` functions, the save menu, the quad renderers, the exit and screenshot screens), and the Xbox Live code.
- `src/cri/`: CRI's Xbox middleware. `src/game/adx.c` and `aix.c` decode natively.
- `Compress/Lz77` and the decomp's zlib copy. The disc uses zlib inflate; the system zlib is linked.
- The XBE. No table is read from it.

What the game logic calls in those files is written here, in `src/port/`, on SDL3, the Vulkan and OpenGL renderer in `src/render/`, and OpenAL. The Xbox C is the reference for what each call does. It is not compiled.

## Stubs

`tools/gen_stubs.py` runs at link time. Every name the game logic needs that neither `src/port/` nor the host libraries define gets a stub in `build/decomp/stubs.c`:

- a function reports its first call (`port: no shim for NAME`) and returns 0;
- data, meaning any name used other than by a call, is a zeroed 64 KiB block;
- a folded name from the decomp's `tools/fold_aliases.py` jumps to the body it was merged into.

The stub list is the native work left. Defining a name in `src/port/` removes its stub on the next build. `SF_PORT_STRICT=1` aborts on the first stubbed call.

## x86-64

The game code keeps some addresses in `u32` words. `sf33rdsxb_game` is linked non-PIE, so code and globals sit below 2 GB, and `sf_low_alloc` (`src/port/lowmem.c`) gives the game heap memory below 2 GB with `MAP_32BIT`. The decomp's `include/` is copied into the build tree with `size_t` and the pointer-sized integers widened. A decomp file that does not compile for x86-64 is listed in `cmake/decomp-overlay.txt` and compiled from a changed copy: `lowaddr` runs `tools/lowaddr_init.py`, which moves address words out of static initializers into a constructor, and `patch` applies a patch from `cmake/decomp-overlay/`. Each such change belongs upstream once the decomp can take it without changing its relink.

## Frame

`src/port/frame.c` is one frame in the order retail's frame function runs it, without the Live calls. `sf_game_init` is its first-frame branch.

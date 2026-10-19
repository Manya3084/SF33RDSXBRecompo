# SF33RDSXBRecompo

x86-64 port of Street Fighter III: 3rd Strike from the original Xbox.

The game data is the Xbox version. On first launch, if `data/afs/SF33RDXB.AFS` is missing, the program asks for one of:

- an Xbox XDVDFS disc image
- a zip or 7z archive of that Xbox disc (the image itself, or the extracted Xbox tree)
- the extracted Xbox folder (`sf33rd.xbe` and `XB_HostPC/SF33RDXB.AFS`)

Anything else is refused, including a PlayStation 2 disc packed in a zip or 7z. The reader looks for the Xbox volume id `MICROSOFT*XBOX*MEDIA` and for `XB_HostPC/SF33RDXB.AFS`. It copies the 3rd Strike archive, the opening AIX, fonts, and NV2A shaders. SF2 movies and the XBE are not copied. During that import, `.xvu` and `.xpu` shaders are converted to one SPIR-V module each. The same words load on Vulkan and on desktop OpenGL.

## Host

SDL3 opens the window. Vulkan is the primary renderer. If it fails, desktop OpenGL 4.6 (compatibility profile, not GLES) is used for that session and the saved choice stays Vulkan so the next launch tries it again. OpenGL 4.6 loads the converted modules with `glShaderBinary`. The UI canvas stays on the fixed-function path. Converted programs use Vulkan clip space (upper-left origin, zero-to-one depth); the canvas restores the ordinary OpenGL clip before it draws. OpenAL opens the audio device. The display menu changes resolution and renderer. Xbox Live is a placeholder screen.

The process heap is mimalloc when the build is configured with `-DSF_USE_MIMALLOC=ON` and mimalloc is installed. Otherwise `sf_alloc` uses the C library. The fight code keeps its own cell heap: `mmHeapCreate` takes the block from `sf_alloc`, and a cell header stays 12 bytes inside the retail 16-byte unit. There is no Xbox heap, XDK, xboxkrnl, D3D8, or XOnline dependency.

Each frame runs the 12-slot task table, draws the shell, and presents once. `cpReadyTask` arms a slot. The next frame runs it. Xbox Live stays the placeholder screen.

When the archive is present, the title screen shows the logo from `TITLE_Init`. `Country` 1 and 2 load `Title_R.ppg` (file 84). Every other value, including the retail boot value 0, loads `TitleTM.ppg` (file 83). Each file is one 512×256 texture. `compress` 2 is zlib inflate. The 16-bit texels are byte-swapped, then A1R5G5B5 expands into the canvas. Vulkan and OpenGL present that canvas with the shell. The disc fragment modules are DOF1 and DOF2, and those combiners write black, so this picture does not sample them.

The same screen shows the first opening tile. `OPBG_Init` reads `Opening.ppg` (file 81) after it has been stored under RAMCNT type `0x1D`. Texture 0 is 256×256 and 8-bit. Its one rectangle covers the tile and selects palette 15. `put_chr2` adds 300 to that byte, and `title.bin` (file 82) is the color load that fills `ColorRAM` from row 300, so the colors are 64 little-endian A1R5G5B5 words in row 15. Pixel 0 is index 28, word `0xEF7B`. The logo and the shell stay. The other 89 tiles and the opening demo are not running.

The title screen plays `o01_A_NYC.adx` (file 87) when the archive is present. It is stereo, 48000 Hz, version 4, 4-bit, with 18-byte blocks and a high-pass of 500. The flags byte is 0, so the scale is the low 13 bits of the frame word plus one. A frame word with bit 15 set ends the stream. The samples go to OpenAL. The filter coefficients stay in double precision. The loop runs from sample 191744 through sample 2875501. A version 3 mix stores that same loop at byte 0x14, so `01_A_NYC.adx` loops from sample 25312 through sample 2699712. The arrow keys change to the next full mix in the archive. Stem files stay out of that list. A, S, D, and F play the first four PCM waves in `SE1.xwb` (file 8) without stopping the music. The opening AIX, the BGM packs, and the sound triggers are not running.

Game logic will be ported from [3s-decomp-ogxb](https://github.com/Manya3084/3s-decomp-ogxb). This repository is the runtime.

## Controls

Xbox layout on a gamepad. Keyboard stand-in: arrows are the d-pad, Z is A, X is B, A is X, S is Y, Enter is Start, Backspace is Back, Q is LB, E is RB. On the data screen, Enter picks a file and F picks a folder. On the title screen, the arrow keys change the music and A, S, D, and F play effects. Start opens the display menu. Back opens the Xbox Live placeholder. Escape quits, or closes the menu that is open.

## Layout

```
data/manifest
data/afs/SF33RDXB.AFS
data/afs/index
data/afs/files/
data/audio/ST3_3_OP.aix
data/fonts/
data/shaders/nv2a/
data/shaders/spirv/
config/settings.txt
```

SPIR-V uses one descriptor set. Vertex binding 0 is 192 vec4 constants, when the program reads them, and binding 1 is the viewport. Fragment binding 2 is 16 vec4 pixel constants plus fog. Fragment bindings 3 through 6 are the four texture stages. The entry point is `main`.

`SF33RDXB.AFS` stays packed, and its members are also written under `data/afs/files/` using the names stored in the archive. `data/afs/index` lists them as `index name bytes`, which is the order `ADXF_OpenAfs` uses. The Xbox archive holds 1531 files. Members are copied as stored, so image pixels stay compressed.

A `.ppg` is a chunk list. `pTEX` and `pPAL` start with a 16-byte header: a big-endian tag, a big-endian size, then raw bytes. The next chunk begins at that size rounded up to 4. `pEND` is the four bytes that end the list and has no size. A `pTEX` width and height count 16-pixel cells. `pixel & 3` selects 4-bit, 8-bit, 16-bit, or 32-bit texels. `formARGB` is `0x1555` (A1R5G5B5), `0x0565` (R5G6B5), `0x4444`, or `0x8888`. Zero means the texels are indexed. `compress & 3` is a raw copy, a zlib wrapper, or zlib inflate. On this disc every `pTEX` uses inflate, and the payload begins with a zlib header. The Xbox function named as LZ77 in the decomp is that inflate. This port does not run the PlayStation 2 LZ77 bitstream. `transNums` is how many 3-byte sprite rectangles sit between the header and the pixels. A `.ppx` is the same list, and each `pCMP` chunk is one compressed block plus its expanded size.

```
sf33rdsxb
sf33rdsxb --extract SOURCE --data DIR
```

`SOURCE` is an Xbox ISO, a zip, a 7z, or an extracted Xbox folder.

## Build

```
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

SDL3, Vulkan, OpenAL, OpenGL, and libarchive are required. libarchive is what reads zip and 7z without pulling in the whole disc. Python 3 runs `tools/spirv.py` while importing shaders.

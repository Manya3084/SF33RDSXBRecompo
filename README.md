# SF33RDSXBRecompo

x86-64 port of Street Fighter III: 3rd Strike from the original Xbox.

The game data is the Xbox version. On first launch, if `data/afs/SF33RDXB.AFS` is missing, the program asks for one of:

- an Xbox XDVDFS disc image
- a zip or 7z archive of that Xbox disc (the image itself, or the extracted Xbox tree)
- the extracted Xbox folder (`sf33rd.xbe` and `XB_HostPC/SF33RDXB.AFS`)

Anything else is refused, including a PlayStation 2 disc packed in a zip or 7z. The reader looks for the Xbox volume id `MICROSOFT*XBOX*MEDIA` and for `XB_HostPC/SF33RDXB.AFS`. It copies the 3rd Strike archive, the opening AIX, fonts, and NV2A shaders. SF2 movies and the XBE are not copied. During that import, `.xvu` and `.xpu` shaders are converted to one SPIR-V module each. The same words load on Vulkan and on desktop OpenGL.

## Host

SDL3 opens the window. Vulkan is the primary renderer. If it fails, desktop OpenGL 4.6 (compatibility profile, not GLES) is used for that session and the saved choice stays Vulkan so the next launch tries it again. OpenGL 4.6 loads the converted modules with `glShaderBinary`. The UI canvas stays on the fixed-function path. Converted programs use Vulkan clip space (upper-left origin, zero-to-one depth); the canvas restores the ordinary OpenGL clip before it draws. OpenAL opens the audio device. The display menu changes resolution and renderer. Xbox Live is a placeholder screen.

The process heap is mimalloc when the build is configured with `-DSF_USE_MIMALLOC=ON` and mimalloc is installed. Otherwise `sf_alloc` uses the C library. The fight code will keep its own work pools. There is no Xbox heap, XDK, xboxkrnl, D3D8, or XOnline dependency.

Game logic will be ported from [3s-decomp-ogxb](https://github.com/Manya3084/3s-decomp-ogxb). This repository is the runtime.

## Controls

Xbox layout on a gamepad. Keyboard stand-in: arrows are the d-pad, Z is A, X is B, A is X, S is Y, Enter is Start, Backspace is Back, Q is LB, E is RB. On the data screen, Enter picks a file and F picks a folder. Start opens the display menu. Back opens the Xbox Live placeholder. Escape quits, or closes the menu that is open.

## Layout

```
data/manifest
data/afs/SF33RDXB.AFS
data/audio/ST3_3_OP.aix
data/fonts/
data/shaders/nv2a/
data/shaders/spirv/
config/settings.txt
```

SPIR-V uses one descriptor set. Vertex binding 0 is 192 vec4 constants, when the program reads them, and binding 1 is the viewport. Fragment binding 2 is 16 vec4 pixel constants plus fog. Fragment bindings 3 through 6 are the four texture stages. The entry point is `main`.

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

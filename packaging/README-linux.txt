SF33RDSXBRecompo: Street Fighter III: 3rd Strike (Xbox), native x86-64
https://github.com/Manya3084/SF33RDSXBRecompo

This package has no game data. You need your own copy of the Xbox disc.

NEEDS
  glibc 2.39 or newer (Ubuntu 24.04, Debian 13, Fedora 40 or later), and:
    Ubuntu/Debian  sudo apt install libopenal1 libarchive13t64 libvulkan1
                   (Debian 12 and older Ubuntu: libarchive13)
    Fedora         sudo dnf install openal-soft libarchive vulkan-loader
    Arch           sudo pacman -S openal libarchive vulkan-icd-loader
  A Vulkan driver, or OpenGL 3.3. SDL3 comes with the package (lib/).

PLAY
  Unpack this folder somewhere you can write to, then run:
    ./sf33rdsxb

  The first time, the window asks for the Xbox disc:
    - click it (or press Enter) and pick the disc image, or a zip or 7z of it;
    - or press F and pick the extracted disc folder (sf33rd.xbe and XB_HostPC);
    - or drop any of those on the window.
  It imports the game into data/ next to the program (about 1.3 GB) and
  starts. After that it starts straight away. A PlayStation 2 disc does not
  work; only the Xbox one.

  The import can also be run from a terminal:
    ./sf33rdsxb --extract /path/to/disc.iso --data .

KEYS (defaults; OPTION > BUTTON CONFIG, F1 rebinds them)
  arrows  d-pad        Z X  A B      A S  X Y      Q  White    E  Black
  W R     L R          Enter Start   Backspace Back
  Gamepads join by pressing Start.

SCREEN
  OPTION > SCREEN ADJUST: window mode, resolution, aspect ratio.
  SF_RENDERER=opengl ./sf33rdsxb uses OpenGL instead of Vulkan.

FILES
  data/    the imported game, and data/save/ (settings, replays)
  config/  the display and keyboard settings

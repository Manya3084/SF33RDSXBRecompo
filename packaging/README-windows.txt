SF33RDSXBRecompo: Street Fighter III: 3rd Strike (Xbox), native x86-64
https://github.com/Manya3084/SF33RDSXBRecompo

This package has no game data. You need your own copy of the Xbox disc.

NEEDS
  64-bit Windows 10 or 11, and a graphics driver with Vulkan or OpenGL 3.3
  (any current AMD, NVIDIA or Intel driver). Everything else is in this
  folder.

PLAY
  1. Unzip the whole folder somewhere you can write to, such as your
     Documents or Desktop (not Program Files: the game keeps its data and
     saves next to sf33rdsxb.exe).
  2. Run sf33rdsxb.exe. The program is not signed, so Windows may show
     "Windows protected your PC": click More info, then Run anyway.
  3. The first time, the window asks for the Xbox disc:
       - click it (or press Enter) and pick the disc image, or a zip or 7z
         of it;
       - or press F and pick the extracted disc folder (sf33rd.xbe and
         XB_HostPC);
       - or drop any of those on the window.
     It imports the game into data\ next to the program (about 1.3 GB) and
     starts. After that it starts straight away. A PlayStation 2 disc does
     not work; only the Xbox one.

  The import can also be run from a command prompt in this folder:
    sf33rdsxb.exe --extract D:\path\to\disc.iso --data .

KEYS (defaults; OPTION > BUTTON CONFIG, F1 rebinds them)
  arrows  d-pad        Z X  A B      A S  X Y      Q  White    E  Black
  W R     L R          Enter Start   Backspace Back
  Gamepads join by pressing Start.

SCREEN
  OPTION > SCREEN ADJUST: window mode, resolution, aspect ratio.
  If the game does not start, set the environment variable
  SF_RENDERER=opengl to use OpenGL instead of Vulkan.

FILES
  data\    the imported game, and data\save\ (settings, replays)
  config\  the display and keyboard settings
  crash.log, in data\, if the game ever stops on an error

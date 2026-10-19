# The game code: the C of 3s-decomp-ogxb, compiled for x86-64.
#
# The decomp checkout is read, never written. Its include/ is copied into
# the build tree so that types.h can give size_t and the pointer-sized
# integers their LP64 widths; every source file compiles from the checkout
# as it is, apart from the few listed in cmake/decomp-overlay.txt, which
# compile from a patched copy.
#
# The game code keeps some addresses in u32 words (PS2 habit). The program
# is linked non-PIE, so every function and global sits below 2 GB and such
# a word holds it exactly; src/port/ keeps the game heap low for the same
# reason.

set(SF_DECOMP_DIR "${CMAKE_SOURCE_DIR}/../3s-decomp-ogxb" CACHE PATH "3s-decomp-ogxb checkout")
get_filename_component(SF_DECOMP_DIR "${SF_DECOMP_DIR}" ABSOLUTE)

if(NOT EXISTS "${SF_DECOMP_DIR}/src/sf33rd/Source/Game")
    message(STATUS "3s-decomp-ogxb not found at ${SF_DECOMP_DIR}; sf_decomp is not built")
    return()
endif()
message(STATUS "3s-decomp-ogxb: ${SF_DECOMP_DIR}")

set(SF_DECOMP_GEN "${CMAKE_BINARY_DIR}/decomp")

# include/, with 64-bit sizes.
file(GLOB_RECURSE _inc RELATIVE "${SF_DECOMP_DIR}/include" "${SF_DECOMP_DIR}/include/*.h")
foreach(h IN LISTS _inc)
    configure_file("${SF_DECOMP_DIR}/include/${h}" "${SF_DECOMP_GEN}/include/${h}" COPYONLY)
endforeach()
file(READ "${SF_DECOMP_GEN}/include/types.h" _types)
# the compiler's own pointer-sized types: long on Linux (LP64), long long
# on Windows (LLP64)
set(_lp64 "typedef __INTPTR_TYPE__ intptr_t;\ntypedef __UINTPTR_TYPE__ uintptr_t;\ntypedef __SIZE_TYPE__ size_t;\ntypedef __PTRDIFF_TYPE__ ssize_t;")
string(REPLACE "typedef s32 intptr_t;\ntypedef u32 uintptr_t;\ntypedef u32 size_t;\ntypedef s32 ssize_t;" "${_lp64}" _types64 "${_types}")
if(_types64 STREQUAL _types)
    message(FATAL_ERROR "decomp include/types.h: the 32-bit size typedefs moved; update cmake/decomp.cmake")
endif()
file(WRITE "${SF_DECOMP_GEN}/include/types.h" "${_types64}")

# Sources: the game logic only. Nothing of the Xbox build is compiled:
# not the platform layer and Xbox-only game code under Source/XBOX (its
# effect draws, save menu and quad renderers are written natively in
# src/), not CRI's Xbox middleware (src/game/adx.c and aix.c decode
# natively), not the PS2 LZ77 (the disc uses zlib), and not the decomp's
# zlib copy (the system's is used).
file(GLOB_RECURSE _src RELATIVE "${SF_DECOMP_DIR}" CONFIGURE_DEPENDS "${SF_DECOMP_DIR}/src/*.c")
list(FILTER _src EXCLUDE REGEX "^src/sf33rd/Source/XBOX/")
list(FILTER _src EXCLUDE REGEX "^src/cri/")
list(FILTER _src EXCLUDE REGEX "^src/sf33rd/Source/Compress/zlib/")
list(FILTER _src EXCLUDE REGEX "^src/sf33rd/Source/Compress/Lz77/")
# zlibApp.c declares a 32-bit z_stream; src/port/zlib.c keeps its API
list(REMOVE_ITEM _src "src/sf33rd/Source/Compress/zlibApp.c")
# MemMan.c keeps two pointers in a cell header that must fit the 16-byte
# unit; src/game/memman.c is the same heap with 32-bit links
list(REMOVE_ITEM _src "src/sf33rd/Source/Common/MemMan.c")

# A file listed in cmake/decomp-overlay.txt compiles from a copy in the
# build tree: "patch PATH" applies cmake/decomp-overlay/<PATH with _>.patch,
# "lowaddr PATH" runs tools/lowaddr_init.py over it. The copy keeps the
# original's directory on its include path.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(_overlay_dir "${CMAKE_SOURCE_DIR}/cmake/decomp-overlay")
file(STRINGS "${CMAKE_SOURCE_DIR}/cmake/decomp-overlay.txt" _overlay REGEX "^[a-z]+ ")
set(SF_DECOMP_SOURCES "")
foreach(s IN LISTS _src)
    set(_how "")
    foreach(line IN LISTS _overlay)
        string(REGEX MATCH "^([a-z]+) (.*)$" _m "${line}")
        if(CMAKE_MATCH_2 STREQUAL s)
            set(_how "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    set(_in "${SF_DECOMP_DIR}/${s}")
    set(_out "${SF_DECOMP_GEN}/overlay/${s}")
    get_filename_component(_odir "${_out}" DIRECTORY)
    get_filename_component(_idir "${_in}" DIRECTORY)
    string(REPLACE "/" "_" _p "${s}")
    if(_how STREQUAL "")
        list(APPEND SF_DECOMP_SOURCES "${_in}")
        continue()
    elseif(_how STREQUAL "patch")
        add_custom_command(OUTPUT "${_out}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_odir}"
            COMMAND ${CMAKE_COMMAND} -E copy "${_in}" "${_out}"
            COMMAND patch --quiet --forward "${_out}" "${_overlay_dir}/${_p}.patch"
            DEPENDS "${_in}" "${_overlay_dir}/${_p}.patch"
            COMMENT "Overlay ${s}")
    elseif(_how STREQUAL "lowaddr")
        add_custom_command(OUTPUT "${_out}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_odir}"
            COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/lowaddr_init.py" "${_in}" "${_out}"
            DEPENDS "${_in}" "${CMAKE_SOURCE_DIR}/tools/lowaddr_init.py"
            COMMENT "Low-address words ${s}")
    else()
        message(FATAL_ERROR "cmake/decomp-overlay.txt: unknown kind ${_how} for ${s}")
    endif()
    set_source_files_properties("${_out}" PROPERTIES INCLUDE_DIRECTORIES "${_idir}")
    list(APPEND SF_DECOMP_SOURCES "${_out}")
endforeach()

add_library(sf_decomp STATIC ${SF_DECOMP_SOURCES})
set_target_properties(sf_decomp PROPERTIES C_STANDARD 17 C_EXTENSIONS ON POSITION_INDEPENDENT_CODE OFF)
target_include_directories(sf_decomp PUBLIC "${SF_DECOMP_GEN}/include")
# The decomp is C written to match retail code, not to pass -Wall; the
# runtime's warnings stay on its own files.
target_compile_options(sf_decomp PRIVATE -g
    -w -Wno-error -fcommon -fno-strict-aliasing -fwrapv -fno-pie
    -Wno-incompatible-pointer-types -Wno-int-conversion -Wno-implicit-function-declaration
    -Wno-implicit-int -Wno-return-mismatch)

# The game: the decomp's game logic on the native platform layer in
# src/port. stubs.c is generated: one logging stub for each name the game
# logic calls that src/port does not define yet, so the list of stubs is
# the native work left. Linked non-PIE so addresses fit u32 words.
# The native platform the game logic runs on. sf_gfx is the graphics
# calls (tested on their own, tests/test_scene.c); sf_port the game's
# frame and window.
add_library(sf_gfx STATIC
    src/port/missing.c
    src/port/lowmem.c
    src/port/gfx.c
)
target_include_directories(sf_gfx PUBLIC src "${SF_DECOMP_GEN}/include")
set_target_properties(sf_gfx PROPERTIES POSITION_INDEPENDENT_CODE OFF)
target_compile_options(sf_gfx PRIVATE -fno-pie)

add_library(sf_port STATIC
    src/port/frame.c
    src/port/video.c
    src/port/system.c
    src/port/zlib.c
    src/port/files.c
    src/port/input.c
    src/port/save.c
    src/port/banks.c
    src/port/math.c
    src/port/sound.c
    src/port/se.c
    src/port/menu_ext.c
    src/port/display.c
    src/port/state.c
    src/port/live.c
    src/game/adx.c
    src/game/aix.c
    src/game/xwb.c
    src/host/paths.c
    src/game/memman.c
    src/mem/heap.c
)
target_link_libraries(sf_port PUBLIC ZLIB::ZLIB OpenAL::OpenAL)
target_link_libraries(sf_port PUBLIC sf_gfx sf_render)
set_target_properties(sf_port PROPERTIES POSITION_INDEPENDENT_CODE OFF)
target_compile_options(sf_port PRIVATE -fno-pie)

# The port's own variables (the window, textures, sound, input devices)
# are not the game's state: their sections are renamed so the linker marks
# them (__start_sfport_data and the rest) and a game-state snapshot
# (src/port/state.c) leaves them out.
if(WIN32)
    find_program(SF_OBJCOPY NAMES x86_64-w64-mingw32-objcopy REQUIRED)
else()
    find_program(SF_OBJCOPY NAMES objcopy REQUIRED)
endif()
foreach(_lib sf_port sf_gfx sf_render)
    add_custom_command(TARGET ${_lib} POST_BUILD
        COMMAND ${SF_OBJCOPY} --rename-section .data=sfport_data --rename-section .bss=sfport_bss $<TARGET_FILE:${_lib}>
        COMMENT "Port state sections in ${_lib}")
endforeach()

# Windows: the MinGW tools, and the import libraries whose names the
# system gives (the C runtime, kernel32, OpenGL, and the DLLs it ships with)
set(_stub_win "")
if(WIN32)
    set(_stub_win --coff --nm x86_64-w64-mingw32-nm --objdump x86_64-w64-mingw32-objdump)
    foreach(_l libmsvcrt.a libmingwex.a libmingw32.a libmoldname.a libkernel32.a libuser32.a libopengl32.a libgcc.a)
        execute_process(COMMAND ${CMAKE_C_COMPILER} -print-file-name=${_l} OUTPUT_VARIABLE _p OUTPUT_STRIP_TRAILING_WHITESPACE)
        list(APPEND _stub_win --host "${_p}")
    endforeach()
    foreach(_l libSDL3.dll.a libvulkan-1.dll.a libopenal.dll.a libz.dll.a)
        list(APPEND _stub_win --host "${SF_WIN_SYSROOT}/lib/${_l}")
    endforeach()
endif()

# The aliases for calls to the bodies retail's linker folded, from the
# port's own archives and the decomp's symbol lists (no Xbox build needed)
if(WIN32)
    set(_fold_nm --nm x86_64-w64-mingw32-nm)
else()
    set(_fold_nm "")
endif()
add_custom_command(OUTPUT "${SF_DECOMP_GEN}/fold.txt"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/fold_aliases.py" ${_fold_nm} "${SF_DECOMP_DIR}"
        "${SF_DECOMP_GEN}/fold.txt" $<TARGET_FILE:sf_decomp> $<TARGET_FILE:sf_port> $<TARGET_FILE:sf_gfx>
        $<TARGET_FILE:sf_render>
    DEPENDS sf_decomp sf_port sf_gfx sf_render "${CMAKE_SOURCE_DIR}/tools/fold_aliases.py"
    COMMENT "Folded-body aliases")

add_custom_command(OUTPUT "${SF_DECOMP_GEN}/stubs.c"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/gen_stubs.py" ${_stub_win}
        --aliases "${SF_DECOMP_GEN}/fold.txt" "${SF_DECOMP_GEN}/stubs.c"
        $<TARGET_FILE:sf_decomp> $<TARGET_FILE:sf_port> $<TARGET_FILE:sf_gfx> $<TARGET_FILE:sf_render>
    DEPENDS sf_decomp sf_port sf_gfx sf_render "${CMAKE_SOURCE_DIR}/tools/gen_stubs.py" "${SF_DECOMP_GEN}/fold.txt"
    COMMENT "Port stubs")

add_executable(sf33rdsxb_game src/port/main.c src/host/paths.c "${SF_DECOMP_GEN}/stubs.c")
target_include_directories(sf33rdsxb_game PRIVATE src)
target_compile_options(sf33rdsxb_game PRIVATE -fno-pie)
set_source_files_properties("${SF_DECOMP_GEN}/stubs.c" PROPERTIES COMPILE_OPTIONS "-Wno-error;-w;-fno-builtin")
if(WIN32)
    # The game keeps addresses in 32-bit words: the image at 4 MB, not
    # moved (no ASLR, no high-entropy addresses), so its code and globals
    # sit low as on Linux; the heap is the low arena (src/port/lowmem.c).
    # A Windows program with no console.
    target_link_options(sf33rdsxb_game PRIVATE -mwindows -static-libgcc -Wl,--image-base=0x400000
        -Wl,--disable-dynamicbase -Wl,--disable-high-entropy-va -Wl,-Map=${CMAKE_BINARY_DIR}/sf33rdsxb_game.map)
    target_link_libraries(sf33rdsxb_game PRIVATE sf_port sf_gfx sf_decomp sf_port sf_gfx sf_render SDL3::SDL3 ZLIB::ZLIB)
else()
    target_link_options(sf33rdsxb_game PRIVATE -no-pie -rdynamic -Wl,-Map=${CMAKE_BINARY_DIR}/sf33rdsxb_game.map)
    # the port libraries twice: the decomp calls into them, and they into it
    target_link_libraries(sf33rdsxb_game PRIVATE sf_port sf_gfx sf_decomp sf_port sf_gfx sf_render SDL3::SDL3 ZLIB::ZLIB m)
endif()

if(NOT WIN32)
add_executable(test_scene tests/test_scene.c)
target_link_libraries(test_scene PRIVATE sf_gfx sf_render)
target_link_options(test_scene PRIVATE -no-pie)
add_test(NAME scene COMMAND test_scene)
set(_lvp "")
foreach(f /usr/share/vulkan/icd.d/lvp_icd.x86_64.json /usr/share/vulkan/icd.d/lvp_icd.json)
    if(EXISTS "${f}" AND _lvp STREQUAL "")
        set(_lvp "${f}")
    endif()
endforeach()
# lavapipe when it is installed, so the test does not depend on the GPU
if(_lvp)
    set_tests_properties(scene PROPERTIES ENVIRONMENT "VK_DRIVER_FILES=${_lvp};VK_ICD_FILENAMES=${_lvp}")
endif()
set_tests_properties(scene PROPERTIES SKIP_RETURN_CODE 77)
endif()

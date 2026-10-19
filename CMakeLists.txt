cmake_minimum_required(VERSION 3.20)
project(SF33RDSXBRecompo C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Werror)
add_compile_definitions(_GNU_SOURCE)

find_package(SDL3 REQUIRED)
find_package(OpenAL REQUIRED)
find_package(Vulkan REQUIRED)
find_package(OpenGL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBARCHIVE IMPORTED_TARGET libarchive)

option(SF_USE_MIMALLOC "Use mimalloc for sf_alloc" OFF)

# Shaders: GLSL compiled to SPIR-V at build time and kept as C arrays.
find_program(GLSLC glslc REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(SF_GEN "${CMAKE_BINARY_DIR}/gen")
set(SF_SHADER_HEADERS "")
foreach(sh scene.vert scene.frag show.vert show.frag copy.frag)
    string(REPLACE "." "_" n "${sh}")
    add_custom_command(OUTPUT "${SF_GEN}/render/${n}_spv.h"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SF_GEN}/render"
        COMMAND ${GLSLC} --target-env=vulkan1.0 -O "${CMAKE_SOURCE_DIR}/shaders/${sh}" -o "${SF_GEN}/render/${n}.spv"
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/spv2h.py" "${SF_GEN}/render/${n}.spv"
            "${SF_GEN}/render/${n}_spv.h" "sf_${n}_spv"
        DEPENDS "${CMAKE_SOURCE_DIR}/shaders/${sh}" "${CMAKE_SOURCE_DIR}/tools/spv2h.py"
        COMMENT "Shader ${sh}")
    list(APPEND SF_SHADER_HEADERS "${SF_GEN}/render/${n}_spv.h")
endforeach()

add_library(sf_render STATIC
    src/render/renderer.c
    src/render/vulkan.c
    src/render/gl.c
    src/render/scene.c
    ${SF_SHADER_HEADERS}
)
target_include_directories(sf_render PUBLIC src "${SF_GEN}")
target_link_libraries(sf_render PUBLIC SDL3::SDL3 Vulkan::Vulkan OpenGL::GL)

add_library(sf_import STATIC
    src/iso/xdvdfs.c
    src/iso/archive.c
    src/iso/afs.c
    src/iso/extract.c
    src/iso/xbe.c
)
target_include_directories(sf_import PUBLIC src)
if(LIBARCHIVE_FOUND)
    target_compile_definitions(sf_import PUBLIC SF_HAVE_LIBARCHIVE)
    target_link_libraries(sf_import PUBLIC PkgConfig::LIBARCHIVE)
endif()

add_executable(sf33rdsxb
    src/main.c
    src/mem/heap.c
    src/game/task.c
    src/game/memman.c
    src/game/fs.c
    src/game/ppg.c
    src/game/title.c
    src/game/opening.c
    src/game/pieces.c
    src/game/adx.c
    src/game/aix.c
    src/game/xwb.c
    src/game/tracks.c
    src/host/paths.c
    src/host/settings.c
    src/platform/input.c
    src/platform/audio.c
    src/render/nv2a.c
    src/ui/ui.c
)
target_include_directories(sf33rdsxb PRIVATE src)
target_link_libraries(sf33rdsxb PRIVATE sf_import sf_render SDL3::SDL3 OpenAL::OpenAL Vulkan::Vulkan OpenGL::GL ZLIB::ZLIB m)
if(SF_USE_MIMALLOC)
    pkg_check_modules(MIMALLOC REQUIRED mimalloc)
    target_compile_definitions(sf33rdsxb PRIVATE SF_USE_MIMALLOC)
    target_include_directories(sf33rdsxb PRIVATE ${MIMALLOC_INCLUDE_DIRS})
    target_link_libraries(sf33rdsxb PRIVATE ${MIMALLOC_LIBRARIES})
endif()

add_executable(test_import tests/test_import.c)
target_include_directories(test_import PRIVATE src)
target_link_libraries(test_import PRIVATE sf_import)
target_compile_definitions(test_import PRIVATE SF_SHADER_TOOL="${CMAKE_SOURCE_DIR}/tools/spirv.py")

add_executable(test_shaders tests/test_shaders.c src/render/nv2a.c)
target_include_directories(test_shaders PRIVATE src)
target_link_libraries(test_shaders PRIVATE SDL3::SDL3 Vulkan::Vulkan OpenGL::GL)
target_compile_definitions(test_shaders PRIVATE SF_SHADER_TOOL="${CMAKE_SOURCE_DIR}/tools/spirv.py")

enable_testing()
add_executable(test_frame tests/test_frame.c src/game/task.c src/game/memman.c src/game/opening.c src/mem/heap.c)
target_include_directories(test_frame PRIVATE src)

add_executable(test_picture tests/test_picture.c src/game/fs.c src/game/ppg.c src/game/title.c src/game/pieces.c src/mem/heap.c)
target_include_directories(test_picture PRIVATE src)
target_link_libraries(test_picture PRIVATE ZLIB::ZLIB)

add_executable(test_adx tests/test_adx.c src/game/adx.c src/game/aix.c src/game/fs.c src/game/xwb.c src/game/tracks.c src/mem/heap.c)
target_include_directories(test_adx PRIVATE src)
target_link_libraries(test_adx PRIVATE m)

add_test(NAME import COMMAND test_import)
add_test(NAME shaders COMMAND test_shaders)
add_test(NAME frame COMMAND test_frame)
add_test(NAME picture COMMAND test_picture)
add_test(NAME adx COMMAND test_adx)

include(cmake/decomp.cmake)

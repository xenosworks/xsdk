# cube: an indexed, textured cube with depth (main.c). Build it with xsdk's toolchain file:
#   cmake -S samples/cube -B build/samples/cube -G Ninja -DCMAKE_TOOLCHAIN_FILE=<xsdk>/cmake/xsdk.cmake
cmake_minimum_required(VERSION 3.25)
project(cube C)

option(SAMPLE_BREAK "Break the sample on purpose, so its check fails" OFF)

add_executable(cube main.c)
set_target_properties(cube PROPERTIES C_STANDARD 17 C_EXTENSIONS ON)
target_compile_options(cube PRIVATE -Wall -Wextra -Werror)
if(SAMPLE_BREAK)
	target_compile_definitions(cube PRIVATE SAMPLE_BREAK)
endif()
# Writes cube_shaders.h, with cube_vert and cube_frag.
xsdk_add_shaders(cube cube.vert cube.frag)
xsdk_add_xex(cube)

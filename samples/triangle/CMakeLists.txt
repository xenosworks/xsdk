# triangle: GLSL shaders through xsdk_add_shaders, and a draw (main.c). Build it with xsdk's toolchain file:
#   cmake -S samples/triangle -B build/samples/triangle -G Ninja -DCMAKE_TOOLCHAIN_FILE=<xsdk>/cmake/xsdk.cmake
cmake_minimum_required(VERSION 3.25)
project(triangle C)

option(SAMPLE_BREAK "Break the sample on purpose, so its check fails" OFF)

add_executable(triangle main.c)
set_target_properties(triangle PROPERTIES C_STANDARD 17 C_EXTENSIONS ON)
target_compile_options(triangle PRIVATE -Wall -Wextra -Werror)
if(SAMPLE_BREAK)
	target_compile_definitions(triangle PRIVATE SAMPLE_BREAK)
endif()
# Writes triangle_shaders.h, with triangle_vert and triangle_frag.
xsdk_add_shaders(triangle triangle.vert triangle.frag)
xsdk_add_xex(triangle)

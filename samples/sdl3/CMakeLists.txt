# sdl3: SDL3's gamepad and audio beside xsdk's drawing (main.c). Build it with xsdk's toolchain file and SDL-xenon's
# build:
#   cmake -S samples/sdl3 -B build/samples/sdl3 -G Ninja -DCMAKE_TOOLCHAIN_FILE=<xsdk>/cmake/xsdk.cmake \
#     -DSDL3_DIR=<SDL-xenon's build>
cmake_minimum_required(VERSION 3.25)
project(sdl3 C)

option(SAMPLE_BREAK "Break the sample on purpose, so its check fails" OFF)

# Without SDL3_DIR the sample isn't built.
find_package(SDL3 CONFIG QUIET)
if(NOT SDL3_FOUND)
	message(STATUS "sdl3: not built, as SDL3 wasn't found; set SDL3_DIR to SDL-xenon's build")
	return()
endif()

add_executable(sdl3 main.c)
set_target_properties(sdl3 PROPERTIES C_STANDARD 17 C_EXTENSIONS ON)
target_compile_options(sdl3 PRIVATE -Wall -Wextra -Werror)
if(SAMPLE_BREAK)
	target_compile_definitions(sdl3 PRIVATE SAMPLE_BREAK)
endif()
target_link_libraries(sdl3 PRIVATE SDL3::SDL3-static)
xsdk_add_xex(sdl3)

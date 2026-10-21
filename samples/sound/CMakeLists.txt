# sound: a tone fed to the kernel's render driver through xsdk_audio_* (main.c). Build it with xsdk's toolchain file:
#   cmake -S samples/sound -B build/samples/sound -G Ninja -DCMAKE_TOOLCHAIN_FILE=<xsdk>/cmake/xsdk.cmake
cmake_minimum_required(VERSION 3.25)
project(sound C)

option(SAMPLE_BREAK "Break the sample on purpose, so its check fails" OFF)

add_executable(sound main.c)
set_target_properties(sound PROPERTIES C_STANDARD 17 C_EXTENSIONS ON)
target_compile_options(sound PRIVATE -Wall -Wextra -Werror)
if(SAMPLE_BREAK)
	target_compile_definitions(sound PRIVATE SAMPLE_BREAK)
endif()
xsdk_add_xex(sound)

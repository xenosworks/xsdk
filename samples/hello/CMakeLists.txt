# hello: the log, the display and a controller (main.c). Build it with xsdk's toolchain file:
#   cmake -S samples/hello -B build/samples/hello -G Ninja -DCMAKE_TOOLCHAIN_FILE=<xsdk>/cmake/xsdk.cmake
cmake_minimum_required(VERSION 3.25)
project(hello C)

option(SAMPLE_BREAK "Break the sample on purpose, so its check fails" OFF)

add_executable(hello main.c)
set_target_properties(hello PROPERTIES C_STANDARD 17 C_EXTENSIONS ON)
target_compile_options(hello PRIVATE -Wall -Wextra -Werror)
if(SAMPLE_BREAK)
	target_compile_definitions(hello PRIVATE SAMPLE_BREAK)
endif()
xsdk_add_xex(hello)

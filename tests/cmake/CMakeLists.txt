# A title built through cmake/xsdk.cmake, to check the toolchain file links titles as the Makefile does and
# xsdk_add_shaders builds HLSL.
cmake_minimum_required(VERSION 3.25)
project(cmake_check CXX)

find_package(Threads REQUIRED)

add_executable(cmake_check cmake_check.cpp)
target_compile_features(cmake_check PRIVATE cxx_std_23)
target_compile_options(cmake_check PRIVATE -Wall -Wextra -Werror)
target_link_libraries(cmake_check PRIVATE Threads::Threads)
set(title_id 5853444F)
set(title_name "xsdk cmake check")
target_compile_definitions(cmake_check PRIVATE TITLE_ID_HEX="${title_id}" TITLE_NAME="${title_name}")
# Writes cmake_check_shaders.h, with quad_vs_hlsl and quad_ps_hlsl.
xsdk_add_shaders(cmake_check quad_vs.hlsl quad_ps.hlsl)
xsdk_add_xex(cmake_check TITLE_ID 0x${title_id} NAME "${title_name}" VERSION 1.2.300.4)

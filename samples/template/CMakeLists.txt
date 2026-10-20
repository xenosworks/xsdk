# A new title for the Xbox 360, built with xsdk (AGENTS.md has the commands and the rules). Rename title below to the
# title's own name: the .xex and its log are named after the target.
cmake_minimum_required(VERSION 3.25)
project(title C)

add_executable(title main.c)
set_target_properties(title PROPERTIES C_STANDARD 17 C_EXTENSIONS ON)
target_compile_options(title PRIVATE -Wall -Wextra -Werror)
# GLSL and HLSL shaders go through xsdk_add_shaders(title shader.vert shader.frag shader_vs.hlsl shader_ps.hlsl), which
# writes title_shaders.h.
xsdk_add_xex(title)

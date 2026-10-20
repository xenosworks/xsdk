# Builds titles with the SDK as its Makefile does: cmake -DCMAKE_TOOLCHAIN_FILE=<xsdk>/cmake/xsdk.cmake, after make.
# Code tells the target apart by the compiler's own __XSDK__, and CMake projects by XSDK.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR powerpc)
set(XSDK TRUE)

get_filename_component(XSDK_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(XSDK_TOOLCHAIN "$ENV{HOME}/xsdk-gcc" CACHE PATH "Where tools/toolchain/build.sh installed the toolchain")
set(XSDK_PREFIX "${XSDK_TOOLCHAIN}/bin/powerpc-xsdk-elf-")
# try_compile projects read this file again, without the cache.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES XSDK_TOOLCHAIN)

set(CMAKE_C_COMPILER "${XSDK_PREFIX}gcc")
set(CMAKE_CXX_COMPILER "${XSDK_PREFIX}g++")
set(CMAKE_ASM_COMPILER "${XSDK_PREFIX}gcc")
set(CMAKE_AR "${XSDK_PREFIX}ar")
set(CMAKE_RANLIB "${XSDK_PREFIX}ranlib")
set(CMAKE_OBJDUMP "${XSDK_PREFIX}objdump")

set(CMAKE_C_FLAGS_INIT "-mcpu=cell -fno-pic")
set(CMAKE_CXX_FLAGS_INIT "-mcpu=cell -fno-pic")
set(CMAKE_ASM_FLAGS_INIT "-mcpu=cell -Wa,-mregnames")
set(CMAKE_C_STANDARD_INCLUDE_DIRECTORIES "${XSDK_ROOT}/include")
set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES "${XSDK_ROOT}/include")
set(CMAKE_ASM_STANDARD_INCLUDE_DIRECTORIES "${XSDK_ROOT}/include")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -nostdlib -T ${XSDK_ROOT}/src/xsdk.ld")
set(CMAKE_USER_MAKE_RULES_OVERRIDE "${CMAKE_CURRENT_LIST_DIR}/xsdk_rules.cmake")

# Every executable, try_compile's included, links the SDK's start-up and library, so checks see what titles get.
# libxsdk.a after libstdc++: what libstdc++ calls that xsdk replaces in newlib, rename among them, comes from xsdk.
set(XSDK_LINK_LIBRARIES "-Wl,--start-group <LINK_LIBRARIES> -lstdc++ ${XSDK_ROOT}/build/libxsdk.a -lc -lm -lgcc -Wl,--end-group")
set(CMAKE_C_LINK_EXECUTABLE
	"<CMAKE_C_COMPILER> <FLAGS> <CMAKE_C_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> ${XSDK_LINK_LIBRARIES}")
set(CMAKE_CXX_LINK_EXECUTABLE
	"<CMAKE_CXX_COMPILER> <FLAGS> <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> ${XSDK_LINK_LIBRARIES}")

# The console loads no shared libraries, so projects that build one unless told otherwise build static ones.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries")

# pthreads are libxsdk's; FindThreads's probe also calls pthread_atfork and pthread_exit, which it doesn't have.
set(CMAKE_HAVE_LIBC_PTHREAD 1)

# Only the toolchain's own headers and libraries, never the host's.
set(CMAKE_FIND_ROOT_PATH "${XSDK_TOOLCHAIN}/powerpc-xsdk-elf")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# pkg-config ignores the root path and would hand projects the host's libraries.
set(ENV{PKG_CONFIG_LIBDIR} "${XSDK_TOOLCHAIN}/powerpc-xsdk-elf/lib/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")

set(XSDK_GLSLANG "${XSDK_TOOLCHAIN}/bin/glslang" CACHE FILEPATH
	"glslang for xsdk_add_shaders; if it isn't there, tools/build_shaders.py finds one itself")

# Compiles a target's GLSL and HLSL files, .vert, .frag, _vs.hlsl and _ps.hlsl, into <target>_shaders.h, which its
# sources include (tools/build_shaders.py); once a target.
function(xsdk_add_shaders target)
	set(header "${CMAKE_CURRENT_BINARY_DIR}/${target}_shaders.h")
	list(TRANSFORM ARGN PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/" REGEX "^[^/]" OUTPUT_VARIABLE sources)
	set(glslang "")
	if(EXISTS "${XSDK_GLSLANG}")
		set(glslang --glslang "${XSDK_GLSLANG}")
	endif()
	add_custom_command(OUTPUT "${header}"
		COMMAND python3 "${XSDK_ROOT}/tools/build_shaders.py" ${glslang} --output "${header}" ${sources}
		DEPENDS ${sources} "${XSDK_ROOT}/tools/build_shaders.py" "${XSDK_ROOT}/build/host/libxsc.so"
		VERBATIM)
	target_sources(${target} PRIVATE "${header}")
	target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
endfunction()

# Packs a title's ELF into <target>.xex beside it, and lists what Xenia would skip, as the Makefile does:
# xsdk_add_xex(target [TITLE_ID 0x5853444B] [NAME "the name the dashboard shows"]). A title ID of its own keeps its
# saves apart from other titles'.
function(xsdk_add_xex target)
	cmake_parse_arguments(PARSE_ARGV 1 XEX "" "TITLE_ID;NAME" "")
	set(options)
	if(DEFINED XEX_TITLE_ID)
		list(APPEND options --title-id "${XEX_TITLE_ID}")
	endif()
	if(DEFINED XEX_NAME)
		list(APPEND options --title-name "${XEX_NAME}")
	endif()
	# The link rule names these outside CMake's view, so a rebuilt SDK would otherwise leave the title linked to the old.
	set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
		"${XSDK_ROOT}/build/libxsdk.a" "${XSDK_ROOT}/src/xsdk.ld")
	add_custom_command(TARGET ${target} POST_BUILD
		COMMAND sh -c "'${CMAKE_OBJDUMP}' -d '$<TARGET_FILE:${target}>' | python3 '${XSDK_ROOT}/tools/xenia_scan.py'"
		COMMAND python3 "${XSDK_ROOT}/tools/elf2xex.py" ${options}
			"$<TARGET_FILE:${target}>" "$<TARGET_FILE_DIR:${target}>/${target}.xex"
		VERBATIM)
endfunction()

cmake_minimum_required(VERSION 3.16)
project(fxstat CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(RESHADE_DIR "" CACHE PATH "Path to a crosire/reshade checkout")
set(SPIRV_HEADERS_DIR "" CACHE PATH "Path to a KhronosGroup/SPIRV-Headers checkout")
set(VKD3D_BUILD "" CACHE PATH "Output directory of build-vkd3d-shader.sh (enables --dxbc)")
set(RESHADEFX_VERSION_NUM "" CACHE STRING "__RESHADE__ value, MAJOR*10000+MINOR*100+REVISION (default: the value in compile.cpp)")

if(NOT EXISTS "${RESHADE_DIR}/source/effect_parser.hpp")
	message(FATAL_ERROR "RESHADE_DIR does not point at a reshade checkout")
endif()
if(NOT EXISTS "${SPIRV_HEADERS_DIR}/include/spirv/unified1/spirv.hpp")
	message(FATAL_ERROR "SPIRV_HEADERS_DIR does not point at a SPIRV-Headers checkout")
endif()

# ---------------------------------------------------------------------------
# reshadefx -- the upstream front end and the code generators that have no
# Windows-only dependencies. effect_codegen_dxbc/dxil need d3dcompiler.h.
# ---------------------------------------------------------------------------

add_library(reshadefx STATIC
	"${RESHADE_DIR}/source/effect_lexer.cpp"
	"${RESHADE_DIR}/source/effect_preprocessor.cpp"
	"${RESHADE_DIR}/source/effect_parser_exp.cpp"
	"${RESHADE_DIR}/source/effect_parser_stmt.cpp"
	"${RESHADE_DIR}/source/effect_symbol_table.cpp"
	"${RESHADE_DIR}/source/effect_expression.cpp"
	"${RESHADE_DIR}/source/effect_codegen_glsl.cpp"
	"${RESHADE_DIR}/source/effect_codegen_hlsl.cpp"
	"${RESHADE_DIR}/source/effect_codegen_spirv.cpp")

target_include_directories(reshadefx PUBLIC
	"${RESHADE_DIR}/source"
	"${SPIRV_HEADERS_DIR}/include/spirv/unified1")

if(NOT MSVC)
	target_compile_options(reshadefx PRIVATE -w)
endif()

# source/effect_preprocessor.cpp calls _wfsopen() with SH_DENYWR but never
# includes <share.h>; MSVC provides it transitively and MinGW does not. Force it
# in rather than patching the upstream source.
if(MINGW)
	target_compile_options(reshadefx PRIVATE -include share.h)
endif()

# ---------------------------------------------------------------------------
# SPIRV-Tools -- linked, not shelled out to. See src/core/optimize.cpp.
# ---------------------------------------------------------------------------

find_path(SPIRV_TOOLS_INCLUDE_DIR spirv-tools/optimizer.hpp)
find_library(SPIRV_TOOLS_OPT_LIB SPIRV-Tools-opt)
find_library(SPIRV_TOOLS_LIB SPIRV-Tools)

if(NOT SPIRV_TOOLS_INCLUDE_DIR OR NOT SPIRV_TOOLS_OPT_LIB OR NOT SPIRV_TOOLS_LIB)
	message(FATAL_ERROR
		"SPIRV-Tools not found. Install it (Debian/Ubuntu: spirv-tools) or set "
		"SPIRV_TOOLS_INCLUDE_DIR, SPIRV_TOOLS_OPT_LIB and SPIRV_TOOLS_LIB.")
endif()

# ---------------------------------------------------------------------------
# libfxstat -- the core. No I/O beyond #include resolution, no printf, no
# process spawning. This is what a WASM build wraps.
# ---------------------------------------------------------------------------

add_library(fxstat_core STATIC
	src/core/compile.cpp
	src/core/optimize.cpp
	src/core/spirv_stats.cpp
	src/core/spirv_patch.cpp
	src/core/spirv_opcodes.gen.cpp
	src/core/dxbc_stats.cpp
	src/core/dxbc_compile.cpp)

target_include_directories(fxstat_core
	PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
	PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
	        "${SPIRV_TOOLS_INCLUDE_DIR}")

if(RESHADEFX_VERSION_NUM)
	target_compile_definitions(fxstat_core PRIVATE RESHADEFX_VERSION_NUM=${RESHADEFX_VERSION_NUM})
endif()

target_link_libraries(fxstat_core PUBLIC reshadefx PRIVATE
	"${SPIRV_TOOLS_OPT_LIB}" "${SPIRV_TOOLS_LIB}")

# --- DXBC compilers -------------------------------------------------------
#
# Windows gets both: D3DCompiler (what ReShade really uses, so the real counts,
# and the only route that handles shader model 3 pixel shaders) and vkd3d if it
# was built. Linux gets vkd3d only -- D3DCompiler is a Windows DLL.

set(HAVE_ANY_DXBC FALSE)

if(WIN32)
	target_sources(fxstat_core PRIVATE src/core/dxbc_compile_d3dcompiler.cpp)
	target_compile_definitions(fxstat_core PUBLIC FXSTAT_HAVE_D3DCOMPILER=1)
	# d3dcompiler_47.dll is loaded at run time, so nothing to link here.
	set(HAVE_ANY_DXBC TRUE)
	message(STATUS "fxstat: DXBC via D3DCompiler enabled (Windows)")
endif()

if(VKD3D_BUILD AND EXISTS "${VKD3D_BUILD}/libvkd3d-shader.a")
	target_sources(fxstat_core PRIVATE src/core/dxbc_compile_vkd3d.cpp)
	target_include_directories(fxstat_core PRIVATE
		"${VKD3D_BUILD}/wine-src/libs/vkd3d/include"
		"${VKD3D_BUILD}/shim")
	target_link_libraries(fxstat_core PRIVATE "${VKD3D_BUILD}/libvkd3d-shader.a")
	if(NOT WIN32)
		target_link_libraries(fxstat_core PRIVATE m pthread)
	endif()
	target_compile_definitions(fxstat_core PUBLIC FXSTAT_HAVE_VKD3D=1)
	set(HAVE_ANY_DXBC TRUE)
	message(STATUS "fxstat: DXBC via vkd3d-shader enabled")
endif()

if(NOT HAVE_ANY_DXBC)
	message(STATUS "fxstat: no DXBC compiler (set VKD3D_BUILD, or build on Windows)")
endif()

# ---------------------------------------------------------------------------
# the command line -- argument parsing, file I/O and formatting only
# ---------------------------------------------------------------------------

add_executable(fxstat
	src/cli/main.cpp
	src/cli/report.cpp
	src/cli/baseline.cpp)

target_include_directories(fxstat PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/cli")
target_link_libraries(fxstat PRIVATE fxstat_core)

# A MinGW build otherwise needs libgcc_s_seh-1.dll and libstdc++-6.dll sitting
# next to the exe. Link them in so the binary is a single file people can drop
# anywhere.
if(MINGW)
	target_link_options(fxstat PRIVATE -static-libgcc -static-libstdc++ -static)
endif()

@echo off
setlocal enabledelayedexpansion
REM ReShade Testing Initiative - Windows build (MinGW-w64).
REM
REM Builds the tools from crosire/reshade's own compiler source, pinned to the
REM tag in RESHADE_VERSION:
REM
REM   reshadefx_cli.exe        crosire's tools\fxc.cpp, unmodified. Equivalent to
REM                            the ReShadeFXC that ships with ReShade, including
REM                            --dxbc via the real D3DCompiler.
REM   reshadefx_cli_fixed.exe  the same, with tools\fxc-fix.py applied (six fixes,
REM                            see docs\upstream\reshade-fxc.md).
REM   reshadefx_rga.exe        per-stage SPIR-V instruction cost, optional RGA driver.
REM   fxstat.exe               SPIR-V + DXBC instruction statistics, baseline/diff.
REM   reshadefx_coverage.exe   how much of a shader corpus compiles to DXBC.
REM
REM All DXBC here comes from d3dcompiler_47.dll, which ships with Windows.
REM vkd3d is not used on Windows.
REM
REM Usage: build_reshade_testing_initiative.bat [output_dir] [--cli] [--cli-fixed]
REM                                             [--rga] [--fxstat] [--coverage]
REM
REM No tool flags = build all of them. Output defaults to .\bin.
REM
REM Needs on PATH: git and a MinGW-w64 g++ (x86_64).
REM   Easiest: https://winlibs.com, or MSYS2: pacman -S mingw-w64-x86_64-gcc
REM Also, depending on what is built:
REM   python          reshadefx_cli_fixed
REM   cmake           fxstat
REM   SPIRV-Tools     fxstat (required), reshadefx_rga --optimize (optional).
REM                   Set SPIRV_TOOLS_DIR to a prefix with include\ and lib\,
REM                   e.g. MSYS2: pacman -S mingw-w64-x86_64-spirv-tools and
REM                   set SPIRV_TOOLS_DIR=C:\msys64\mingw64
REM
REM Downloaded dependencies are cached in .\.deps (gitignored; override with
REM DEPS_DIR). Delete it to force a clean fetch.

set "SCRIPT_DIR=%~dp0"
set /p RESHADE_REF=<"%SCRIPT_DIR%RESHADE_VERSION"
for /f "tokens=* delims= " %%a in ("%RESHADE_REF%") do set "RESHADE_REF=%%a"
if not defined DEPS_DIR set "DEPS_DIR=%SCRIPT_DIR%.deps"

set "OUT_DIR="
set "WANT_CLI=0"
set "WANT_CLI_FIXED=0"
set "WANT_RGA=0"
set "WANT_FXSTAT=0"
set "WANT_COVERAGE=0"
set "ANY_SELECTED=0"

:parse_args
if "%~1"=="" goto args_done
if /I "%~1"=="--cli"       ( set "WANT_CLI=1"       & set "ANY_SELECTED=1" & shift & goto parse_args )
if /I "%~1"=="--cli-fixed" ( set "WANT_CLI_FIXED=1" & set "ANY_SELECTED=1" & shift & goto parse_args )
if /I "%~1"=="--rga"       ( set "WANT_RGA=1"       & set "ANY_SELECTED=1" & shift & goto parse_args )
if /I "%~1"=="--fxstat"    ( set "WANT_FXSTAT=1"    & set "ANY_SELECTED=1" & shift & goto parse_args )
if /I "%~1"=="--coverage"  ( set "WANT_COVERAGE=1"  & set "ANY_SELECTED=1" & shift & goto parse_args )
set "ARG=%~1"
if "!ARG:~0,1!"=="-" (
	echo ERROR: unknown option: %~1
	exit /b 1
)
set "OUT_DIR=%~1"
shift
goto parse_args
:args_done

if "%ANY_SELECTED%"=="0" (
	set "WANT_CLI=1"
	set "WANT_CLI_FIXED=1"
	set "WANT_RGA=1"
	set "WANT_FXSTAT=1"
	set "WANT_COVERAGE=1"
)
if "%OUT_DIR%"=="" set "OUT_DIR=%SCRIPT_DIR%bin"

REM --- checks ----------------------------------------------------------------

where g++ >nul 2>nul || ( echo ERROR: g++ not found on PATH. Install MinGW-w64 first. & exit /b 1 )
where git >nul 2>nul || ( echo ERROR: git not found on PATH. & exit /b 1 )
if "%WANT_CLI_FIXED%"=="1" (
	where python >nul 2>nul || ( echo ERROR: reshadefx_cli_fixed needs python on PATH. & exit /b 1 )
)
set "HAVE_SPIRV_TOOLS=0"
if defined SPIRV_TOOLS_DIR if exist "%SPIRV_TOOLS_DIR%\include\spirv-tools\optimizer.hpp" set "HAVE_SPIRV_TOOLS=1"
if "%WANT_FXSTAT%"=="1" (
	if "%HAVE_SPIRV_TOOLS%"=="0" (
		echo ERROR: fxstat needs SPIRV-Tools. Set SPIRV_TOOLS_DIR, see the top of this script.
		echo        To build without it, pick tools explicitly, e.g. --cli --cli-fixed --rga --coverage
		exit /b 1
	)
	where cmake >nul 2>nul || ( echo ERROR: fxstat needs cmake on PATH. & exit /b 1 )
)

if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"
for %%I in ("%OUT_DIR%") do set "OUT_DIR=%%~fI"
if not exist "%DEPS_DIR%" mkdir "%DEPS_DIR%"
for %%I in ("%DEPS_DIR%") do set "DEPS_DIR=%%~fI"
set "WORK_DIR=%TEMP%\rti_build_%RANDOM%%RANDOM%"
mkdir "%WORK_DIR%\res"

REM --- reshade + SPIR-V headers, pinned --------------------------------------

set "R=%DEPS_DIR%\reshade-%RESHADE_REF%"
if not exist "%R%\source\effect_parser.hpp" (
	echo Fetching reshade %RESHADE_REF% ...
	if exist "%R%" rmdir /s /q "%R%"
	git -c advice.detachedHead=false clone --quiet --depth 1 --branch %RESHADE_REF% https://github.com/crosire/reshade.git "%R%" || exit /b 1
)
if not exist "%R%\deps\spirv\include\spirv\unified1\spirv.hpp" (
	echo Fetching SPIR-V headers, the commit reshade %RESHADE_REF% pins ...
	git -C "%R%" submodule update --quiet --init --depth 1 deps/spirv || exit /b 1
)

REM Parse "vMAJOR.MINOR.REV" for --version text and __RESHADE__.
set "VERSTR=%RESHADE_REF%"
if "%VERSTR:~0,1%"=="v" set "VERSTR=%VERSTR:~1%"
set "VER_MAJOR=0"
set "VER_MINOR=0"
set "VER_REV=0"
for /f "tokens=1,2,3 delims=." %%a in ("%VERSTR%") do (
	set "VER_MAJOR=%%a"
	set "VER_MINOR=%%b"
	set "VER_REV=%%c"
)
set /a "VER_NUM=VER_MAJOR*10000 + VER_MINOR*100 + VER_REV"

REM res\version.h is generated by the Visual Studio build. Written to the work
REM dir, not the checkout.
set "VH=%WORK_DIR%\res\version.h"
> "%VH%" echo #pragma once
>>"%VH%" echo #define VERSION_MAJOR %VER_MAJOR%
>>"%VH%" echo #define VERSION_MINOR %VER_MINOR%
>>"%VH%" echo #define VERSION_REVISION %VER_REV%
>>"%VH%" echo #define VERSION_STRING_PRODUCT "ReShade %VER_MAJOR%.%VER_MINOR%.%VER_REV% (ReShade Testing Initiative build)"

REM --- shared compile settings -----------------------------------------------

set "INCLUDES=-I "%R%\source" -I "%WORK_DIR%\res" -I "%R%\deps\spirv\include\spirv\unified1""
set "FX_SRC="%R%\source\effect_lexer.cpp" "%R%\source\effect_preprocessor.cpp" "%R%\source\effect_parser_exp.cpp" "%R%\source\effect_parser_stmt.cpp" "%R%\source\effect_symbol_table.cpp" "%R%\source\effect_expression.cpp""
set "CODEGEN_SRC="%R%\source\effect_codegen_hlsl.cpp" "%R%\source\effect_codegen_glsl.cpp" "%R%\source\effect_codegen_spirv.cpp" "%R%\source\effect_codegen_dxbc.cpp""

REM Matches the size-relevant settings from crosire's own Release|x64 build of
REM ReShadeFXC.vcxproj (FunctionLevelLinking+OptimizeReferences ->
REM -ffunction-sections/-fdata-sections + --gc-sections; ExceptionHandling=false
REM -> -fno-exceptions; GenerateDebugInformation=false -> -s). -static so the
REM .exe needs no MinGW runtime DLLs.
REM share.h provides SH_DENYWR, which MSVC headers pull in transitively and
REM MinGW does not (see docs\CHECKLIST.md, item 5).
set "FLAGS=-std=c++17 -Os -fno-exceptions -ffunction-sections -fdata-sections -s -static -include share.h"
set "LINK_FLAGS=-Wl,--gc-sections"
set "BUILT="

REM --- reshadefx_cli (upstream, unmodified) ----------------------------------

if "%WANT_CLI%"=="1" (
	echo Building reshadefx_cli.exe ...
	g++ %FLAGS% %INCLUDES% %FX_SRC% %CODEGEN_SRC% "%R%\tools\fxc.cpp" %LINK_FLAGS% -ld3dcompiler -o "%OUT_DIR%\reshadefx_cli.exe" || exit /b 1
	set "BUILT=!BUILT! reshadefx_cli.exe"
)

REM --- reshadefx_cli_fixed (fxc-fix.py applied) ------------------------------

if "%WANT_CLI_FIXED%"=="1" (
	echo Building reshadefx_cli_fixed.exe ...
	mkdir "%WORK_DIR%\fixed\tools"
	copy /y "%R%\tools\fxc.cpp" "%WORK_DIR%\fixed\tools\fxc.cpp" >nul
	python "%SCRIPT_DIR%tools\fxc-fix.py" "%WORK_DIR%\fixed" || exit /b 1
	g++ %FLAGS% %INCLUDES% %FX_SRC% %CODEGEN_SRC% "%WORK_DIR%\fixed\tools\fxc.cpp" %LINK_FLAGS% -ld3dcompiler -o "%OUT_DIR%\reshadefx_cli_fixed.exe" || exit /b 1
	set "BUILT=!BUILT! reshadefx_cli_fixed.exe"
)

REM --- reshadefx_coverage ------------------------------------------------------

if "%WANT_COVERAGE%"=="1" (
	echo Building reshadefx_coverage.exe ...
	g++ %FLAGS% -DRESHADEFX_VERSION_NUM=%VER_NUM% %INCLUDES% %FX_SRC% %CODEGEN_SRC% "%SCRIPT_DIR%cli\coverage.cpp" %LINK_FLAGS% -ld3dcompiler -o "%OUT_DIR%\reshadefx_coverage.exe" || exit /b 1
	set "BUILT=!BUILT! reshadefx_coverage.exe"
)

REM --- reshadefx_rga -------------------------------------------------------------

if "%WANT_RGA%"=="1" (
	echo Building reshadefx_rga.exe ...
	set "SPIRV_TOOLS_FLAGS="
	if "%HAVE_SPIRV_TOOLS%"=="1" (
		set "SPIRV_TOOLS_FLAGS=-DRESHADEFX_HAVE_SPIRV_TOOLS -I"%SPIRV_TOOLS_DIR%\include" -L"%SPIRV_TOOLS_DIR%\lib" -lSPIRV-Tools-opt -lSPIRV-Tools"
		echo   SPIRV-Tools found: --optimize will be available
	) else (
		echo   SPIRV-Tools not found: --optimize will report it is unavailable. Set SPIRV_TOOLS_DIR to enable it.
	)
	g++ %FLAGS% -DRESHADEFX_VERSION_NUM=%VER_NUM% %INCLUDES% %FX_SRC% "%R%\source\effect_codegen_spirv.cpp" "%SCRIPT_DIR%rga\reshadefx_rga.cpp" "%SCRIPT_DIR%rga\spirv_optimize.cpp" !SPIRV_TOOLS_FLAGS! %LINK_FLAGS% -o "%OUT_DIR%\reshadefx_rga.exe" || exit /b 1
	set "BUILT=!BUILT! reshadefx_rga.exe"
)

REM --- fxstat --------------------------------------------------------------------

if "%WANT_FXSTAT%"=="1" (
	echo Building fxstat.exe ...
	cmake -G "MinGW Makefiles" -S "%SCRIPT_DIR%fxstat" -B "%WORK_DIR%\fxstat-build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%SPIRV_TOOLS_DIR%" -DRESHADE_DIR="%R%" -DSPIRV_HEADERS_DIR="%R%\deps\spirv" -DRESHADEFX_VERSION_NUM=%VER_NUM% >nul || exit /b 1
	cmake --build "%WORK_DIR%\fxstat-build" --parallel >nul || exit /b 1
	copy /y "%WORK_DIR%\fxstat-build\fxstat.exe" "%OUT_DIR%\fxstat.exe" >nul
	set "BUILT=!BUILT! fxstat.exe"
)

rmdir /s /q "%WORK_DIR%"

echo.
echo Done (ReShade %RESHADE_REF%). Built into %OUT_DIR%:
for %%b in (%BUILT%) do echo   %%b

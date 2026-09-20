/*
 * fxstat command line.
 *
 * Argument parsing, file I/O and output formatting only. Everything that
 * actually compiles or measures lives in the library, so the same core serves a
 * WASM build with a different shell around it.
 */

#include "fxstat/fxstat.hpp"
#include "baseline.hpp"
#include "report.hpp"
#include "rga.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace
{

void print_usage(const char *argv0)
{
	std::printf(
R"(fxstat -- instruction statistics for ReShade effects

usage: %s [options] <effect.fx>

Input:
  -I <path>              Add an include search path (repeatable).
  -D <name>[=<value>]    Define a preprocessor macro (repeatable).
  --preset <file.ini>    ReShade preset to take uniform values from.
  --effect-name <name>   Preset section to read (default: the .fx file name).
  --width <n>            BUFFER_WIDTH  (default 1920).
  --height <n>           BUFFER_HEIGHT (default 1080).

Code generation:
  --spirv                SPIR-V backend (default).
  --dxbc                 DXBC backend (DX9/10/11/12), via vkd3d-shader.
                         Counts the exact bytecode ReShade hands the driver.
  --hlsl                 HLSL backend (generates code; no counting).
  --glsl                 GLSL backend (generates code; no counting).
  --shader-model <n>     HLSL shader model: 30, 40, 41, 50, 51 (default 50).
  --dxbc-compiler <c>    d3dcompiler | vkd3d | auto (default auto). D3DCompiler
                         is what ReShade uses, so its counts are the real ones,
                         but it is Windows-only. Never compare counts from one
                         compiler against the other.
  -O{0,1,2,3} / -Od      D3DCompiler optimisation level (default -O3, what the
                         runtime uses). vkd3d-shader ignores it.
  --renderer <id>        Override __RENDERER__ (e.g. 0xc000 for D3D12). By
                         default it follows the backend and shader model.
                         Use this to audit which path an effect takes per API.
  --performance-mode     Uniforms become constants (default).
  --no-performance-mode  Keep uniforms as uniforms.

Analysis:
  --optimize             Fold constants and remove dead branches before counting
                         -- what the driver does. Default on. SPIR-V only.
  --no-optimize          Count raw ReShadeFX output.

GPU ISA (SPIR-V back end only, needs AMD's Radeon GPU Analyzer):
  --rga <path>           Also compile each entry point with RGA and report the
                         real GPU instructions: VALU, transcendentals, scalar
                         ALU, texture fetches, scratch memory and registers.
                         RGA runs AMD's Vulkan driver compiler offline, no GPU
                         needed. Not bundled: github.com/GPUOpen-Tools/
                         radeon_gpu_analyzer/releases
  --asic <name>          GPU to compile for (default gfx1100, RDNA3).
                         `rga -s vk-spv-offline -l` lists them.

Comparison:
  --baseline <file.json> Compare against a report written earlier by --json.
  --fail-on-regression   Exit 2 if anything got more expensive. For CI. With
                         --rga on both sides, judged on the GPU ISA only.

Output:
  --json                 Machine-readable output.
  --verbose              Per-opcode breakdown.
  --dump <dir>           Write generated code, binaries and disassembly there.
  --list-passes          Print the SPIR-V optimisation pass list and exit.
  --version              Print version information and exit.
  -h, --help             This message.
)", argv0);
}

std::string read_file(const std::string &path, bool &ok)
{
	std::ifstream file(path, std::ios::binary);
	if (!file)
	{
		ok = false;
		return {};
	}
	std::stringstream ss;
	ss << file.rdbuf();
	ok = true;
	return ss.str();
}

} // namespace

int main(int argc, char *argv[])
{
	fxstat::compile_options options;
	std::string source_file, preset_file, effect_section, baseline_file, dump_dir;
	bool json = false, verbose = false, fail_on_regression = false;
	fxstat::rga_options rga;

	for (int i = 1; i < argc; ++i)
	{
		const std::string arg = argv[i];
		auto next = [&]() -> std::string {
			if (i + 1 >= argc)
			{
				std::fprintf(stderr, "error: %s needs a value\n", arg.c_str());
				std::exit(1);
			}
			return argv[++i];
		};

		if (arg == "-h" || arg == "--help")             { print_usage(argv[0]); return 0; }
		else if (arg == "--version")
		{
			std::printf("fxstat\nSPIRV-Tools: %s\nDXBC compilers:", fxstat::optimizer_version().c_str());
			const auto compilers = fxstat::available_dxbc_compilers();
			if (compilers.empty())
				std::printf(" none");
			for (const fxstat::dxbc_compiler c : compilers)
				std::printf(" %s", fxstat::dxbc_compiler_name(c));
			std::printf("\n");
			return 0;
		}
		else if (arg == "--list-passes")
		{
			for (const std::string &p : fxstat::driver_pass_list())
				std::printf("%s\n", p.c_str());
			return 0;
		}
		else if (arg == "-I")                           { options.include_paths.push_back(next()); }
		else if (arg == "-D")
		{
			const std::string def = next();
			const size_t eq = def.find('=');
			if (eq == std::string::npos) options.defines.emplace_back(def, "1");
			else options.defines.emplace_back(def.substr(0, eq), def.substr(eq + 1));
		}
		else if (arg == "--preset")                     { preset_file = next(); }
		else if (arg == "--effect-name")                { effect_section = next(); }
		else if (arg == "--width")                      { options.width = std::stoul(next()); }
		else if (arg == "--height")                     { options.height = std::stoul(next()); }
		else if (arg == "--spirv")                      { options.target = fxstat::backend::spirv; }
		else if (arg == "--dxbc")                       { options.target = fxstat::backend::dxbc; }
		else if (arg == "--hlsl")                       { options.target = fxstat::backend::hlsl; }
		else if (arg == "--glsl")                       { options.target = fxstat::backend::glsl; }
		else if (arg == "--shader-model")               { options.shader_model = std::stoul(next()); }
		else if (arg == "--dxbc-compiler")
		{
			const std::string c = next();
			if (c == "vkd3d")            options.dxbc = fxstat::dxbc_compiler::vkd3d;
			else if (c == "d3dcompiler") options.dxbc = fxstat::dxbc_compiler::d3dcompiler;
			else if (c == "auto")        options.dxbc = fxstat::dxbc_compiler::automatic;
			else { std::fprintf(stderr, "error: unknown DXBC compiler '%s'\n", c.c_str()); return 1; }
		}
		else if (arg == "-Od")                          { options.optimization_level = -1; }
		else if (arg == "-O0")                          { options.optimization_level = 0; }
		else if (arg == "-O1")                          { options.optimization_level = 1; }
		else if (arg == "-O2")                          { options.optimization_level = 2; }
		else if (arg == "-O3")                          { options.optimization_level = 3; }
		else if (arg == "--renderer")                   { options.renderer = std::stoul(next(), nullptr, 0); }
		else if (arg == "--performance-mode")           { options.performance_mode = true; }
		else if (arg == "--no-performance-mode")        { options.performance_mode = false; }
		else if (arg == "--optimize")                   { options.optimize = true; }
		else if (arg == "--no-optimize")                { options.optimize = false; }
		else if (arg == "--rga")                        { rga.executable = next(); }
		else if (arg == "--asic")                       { rga.asic = next(); }
		else if (arg == "--baseline")                   { baseline_file = next(); }
		else if (arg == "--fail-on-regression")         { fail_on_regression = true; }
		else if (arg == "--json")                       { json = true; }
		else if (arg == "--verbose")                    { verbose = true; }
		else if (arg == "--dump")
		{
			dump_dir = next();
			options.keep_generated_code = true;
			options.keep_binaries = true;
		}
		else if (!arg.empty() && arg[0] == '-')
		{
			std::fprintf(stderr, "error: unknown option '%s'\n", arg.c_str());
			return 1;
		}
		else
		{
			source_file = arg;
		}
	}

	if (source_file.empty())
	{
		print_usage(argv[0]);
		return 1;
	}

	if (!preset_file.empty())
	{
		bool ok = false;
		const std::string ini = read_file(preset_file, ok);
		if (!ok)
		{
			std::fprintf(stderr, "error: could not open preset '%s'\n", preset_file.c_str());
			return 1;
		}
		std::string section = effect_section;
		if (section.empty())
			section = std::filesystem::u8path(source_file).filename().u8string();
		options.preset = fxstat::parse_preset(ini, section);
	}

	if (!rga.executable.empty())
	{
		if (options.target != fxstat::backend::spirv)
		{
			std::fprintf(stderr, "error: --rga needs the SPIR-V back end\n");
			return 1;
		}
		options.keep_binaries = true;
	}

	const fxstat::compile_result result = fxstat::compile_file(source_file, options);

	if (!result.warnings.empty())
		std::fputs(result.warnings.c_str(), stderr);
	if (!result.ok)
	{
		std::fputs(result.errors.c_str(), stderr);
		std::fprintf(stderr, "error: compilation failed\n");
		return 1;
	}

	if (!dump_dir.empty())
	{
		std::error_code ec;
		std::filesystem::create_directories(std::filesystem::u8path(dump_dir), ec);
		fxstat::write_dump(dump_dir, source_file, options, result);
	}

	fxstat::isa_map isa;
	const fxstat::isa_map *isa_ptr = nullptr;
	if (!rga.executable.empty())
	{
		isa = fxstat::run_rga(rga, result);
		isa_ptr = &isa;
	}

	if (json)
	{
		fxstat::print_json(stdout, source_file, options, result, isa_ptr, rga.asic);
		return 0;
	}

	if (!baseline_file.empty())
	{
		std::map<std::string, fxstat::baseline_entry> baseline;
		std::string error;
		if (!fxstat::load_baseline(baseline_file, baseline, error))
		{
			std::fprintf(stderr, "error: %s\n", error.c_str());
			return 1;
		}
		std::printf("%s  vs  %s\n", source_file.c_str(), baseline_file.c_str());
		const bool regressed = fxstat::print_diff(stdout, result, baseline, isa_ptr);
		if (regressed && fail_on_regression)
		{
			std::fprintf(stderr, "error: the effect got more expensive against the baseline\n");
			return 2;
		}
		return 0;
	}

	fxstat::print_report(stdout, source_file, options, result, verbose, isa_ptr, rga.asic);
	return 0;
}

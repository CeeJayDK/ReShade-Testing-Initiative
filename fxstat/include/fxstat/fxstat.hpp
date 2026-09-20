/*
 * fxstat -- compile ReShade effects and measure them.
 *
 * This is the whole public surface. Everything here is free of I/O beyond what
 * the ReShadeFX preprocessor does for #include resolution, and free of process
 * spawning, printf and exit(), so the same code serves the CLI, a WASM build
 * and anything else.
 *
 * For a WASM build: preload the shader pack into Emscripten's MEMFS and pass
 * the mount point in `include_paths`. compile_source() lets an editor feed the
 * main effect in from memory without touching a filesystem at all.
 */
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace fxstat
{

// ---------------------------------------------------------------------------
// instruction classification
// ---------------------------------------------------------------------------

enum class category : uint8_t
{
	tex,     // texture sampling / image access
	alu,     // arithmetic, logic, compare, convert, extended instructions
	mov,     // composite construct/extract/insert, swizzles, copies
	mem,     // load / store / access chain / local variable
	flow,    // branches, loops, phi, function calls, returns
	other,
	count_
};

const char *category_name(category c);

struct shader_stats
{
	uint32_t counts[static_cast<size_t>(category::count_)] = {};
	uint32_t total = 0;      // instructions, declarations excluded
	uint32_t functions = 0;
	std::map<std::string, uint32_t> by_opcode;

	// SPIR-V only. The counts above are per instruction, but every GPU since
	// about 2012 runs a float4 operation as four scalar ones, so replacing four
	// vector sin() with one scalar sin() looks like no change in `alu` while it
	// is a 4x saving on the hardware. These weight each ALU instruction by the
	// component count of its result type.
	//
	//   alu_lanes    ALU instructions x components
	//   trans        transcendental operations: sin, cos, exp, log, sqrt,
	//                rsqrt, and the reciprocal inside a division. pow counts 2
	//                (log + exp), tan 3 (sin, cos, divide). These run at quarter
	//                rate on current AMD and NVIDIA hardware.
	//   trans_lanes  the same, x components
	//
	// Still an estimate of what the driver emits; --rga gives the real ISA.
	uint32_t alu_lanes = 0;
	uint32_t trans = 0;
	uint32_t trans_lanes = 0;

	uint32_t get(category c) const { return counts[static_cast<size_t>(c)]; }
};

// ---------------------------------------------------------------------------
// compilation
// ---------------------------------------------------------------------------

enum class backend { spirv, hlsl, glsl, dxbc };

// Which compiler turns the generated HLSL into DXBC.
//
//   d3dcompiler  Windows only, ships with the OS. What ReShade actually uses,
//                so these are the real counts, and the only route that handles
//                shader model 3 pixel shaders.
//   vkd3d        Any platform; the only DXBC route on Linux. Has no equivalent
//                of D3DCOMPILE_OPTIMIZATION_LEVEL*, so its counts differ from
//                D3DCompiler's -- never compare one against the other.
//
// `automatic` prefers D3DCompiler where the build has it. Whichever ran is
// recorded in compile_result::dxbc_compiler_used.
enum class dxbc_compiler { automatic, vkd3d, d3dcompiler };

std::vector<dxbc_compiler> available_dxbc_compilers();
const char *dxbc_compiler_name(dxbc_compiler c);

// Renderer IDs as source/runtime.cpp assigns them. These decide which
// __RENDERER__ branch an effect takes, which is frequently not the same code.
enum renderer_id : unsigned int
{
	renderer_d3d9    = 0x9000,
	renderer_d3d10   = 0xa000,
	renderer_d3d10_1 = 0xa100,
	renderer_d3d11   = 0xb000,
	renderer_d3d11_1 = 0xb100,
	renderer_d3d12   = 0xc000,
	renderer_opengl  = 0x10000,
	renderer_vulkan  = 0x20000,
};

struct compile_options
{
	backend target = backend::spirv;
	unsigned int shader_model = 50;
	dxbc_compiler dxbc = dxbc_compiler::automatic;

	// D3DCompiler optimisation level: -1 disables, 0-3 select a level. The
	// runtime uses 3 in performance mode. vkd3d-shader ignores it.
	int optimization_level = 3;

	// Uniforms become constants, so the branches they select between can be
	// eliminated. Statistics in this mode are a property of the preset.
	bool performance_mode = true;

	// Run the folding / mem2reg / dead-branch passes a driver runs before
	// counting. Only meaningful for the SPIR-V back end; the DXBC back end is
	// optimised by vkd3d before it ever reaches us.
	bool optimize = true;

	unsigned int width = 1920;
	unsigned int height = 1080;

	// 0 means "derive from target and shader_model". Set it explicitly to audit
	// which path an effect takes on a given API.
	unsigned int renderer = 0;

	std::vector<std::string> include_paths;
	std::vector<std::pair<std::string, std::string>> defines;

	// Uniform name -> raw preset value ("2", "0.65", "1.0,0.5,0.25"). Applied
	// only in performance mode. Use parse_preset() to build it from ini text.
	std::map<std::string, std::string> preset;

	// Keep the generated HLSL/GLSL and the per-entry-point binary and
	// disassembly in the result. Off by default: they are large.
	bool keep_generated_code = false;
	bool keep_binaries = false;
};

struct entry_point_result
{
	std::string name;
	std::string stage;            // "vertex", "pixel", "compute"

	shader_stats stats;           // what the GPU actually runs
	shader_stats stats_unoptimized;
	bool has_unoptimized = false; // false for DXBC: vkd3d already optimised it

	std::string profile;          // DXBC only, e.g. "ps_5_0"
	uint32_t temp_registers = 0;  // DXBC only, from dcl_temps

	// SPIR-V only. The SPIRV-Tools inliner declines some modules; when it does,
	// mem2reg has nothing to work on, the load/store scaffolding survives, and
	// the counts come out inflated -- on SMAA by a factor of five. Detected by
	// OpFunctionParameter surviving optimisation. Treat the counts as unusable.
	bool inliner_incomplete = false;

	std::string binary;           // .cso or .spv, if keep_binaries
	std::string assembly;         // d3d-asm, if available and keep_binaries
};

struct compile_result
{
	bool ok = false;
	std::string errors;
	std::string warnings;

	std::vector<entry_point_result> entry_points;

	// The uniform values actually baked in, in order, for reporting. Empty when
	// performance mode is off.
	std::vector<std::pair<std::string, std::string>> uniform_values;

	std::string generated_code;   // if keep_generated_code

	// What was actually used, for reproducibility in reports.
	unsigned int renderer_used = 0;
	std::string optimizer_version;

	// Which DXBC compiler produced these numbers, e.g. "d3dcompiler_47.dll" or
	// "vkd3d-shader". Empty for non-DXBC back ends. A DXBC report that does not
	// carry this cannot be compared with another machine's.
	std::string dxbc_compiler_used;
};

compile_result compile_file(const std::string &path, const compile_options &options);
compile_result compile_source(const std::string &source, const std::string &name,
                              const compile_options &options);

// ---------------------------------------------------------------------------
// presets
// ---------------------------------------------------------------------------

// Parse ReShade preset ini text. `section` is normally the effect's file name;
// an empty section takes keys from every section, last one winning.
std::map<std::string, std::string> parse_preset(const std::string &ini_text,
                                                const std::string &section);

// ---------------------------------------------------------------------------
// misc
// ---------------------------------------------------------------------------

// Renderer ID the runtime would report for a back end and shader model.
unsigned int default_renderer(backend target, unsigned int shader_model);

const char *backend_name(backend b);

// The SPIR-V optimisation passes applied when `optimize` is set. Deliberately
// not spirv-opt -O; see src/core/optimize.cpp for why. Exposed so a report can
// record exactly what produced its numbers.
std::vector<std::string> driver_pass_list();

// Version of the linked SPIRV-Tools. Counts drift between releases, so a report
// that does not record this cannot be compared against another machine's.
std::string optimizer_version();

} // namespace fxstat

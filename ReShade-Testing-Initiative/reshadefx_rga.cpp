// reshadefx_rga: makes RGA (Radeon GPU Analyzer) understand ReShade FX shaders.
//
// RGA only takes raw HLSL/GLSL/SPIR-V - it has no idea what a ReShade FX
// "technique"/"pass" is, or how to resolve ReShade.fxh. This tool compiles a
// .fx file with the real reshadefx frontend (same source as the in-game
// compiler), extracts each shader stage as standalone SPIR-V, and:
//
//   1. Always: classifies the real SPIR-V instruction stream into cost
//      buckets (cheap ALU / transcendental / texture / control flow), a
//      vendor-general proxy that works with no other tools installed.
//   2. If --rga and --asic are given: feeds each stage's SPIR-V into RGA
//      to get genuine GPU ISA and real VGPR/SGPR usage for a real AMD
//      target, instead of a heuristic.
//
// Built from crosire/reshade's unmodified source/effect_*.cpp files.
#include "effect_parser.hpp"
#include "effect_codegen.hpp"
#include "effect_preprocessor.hpp"
#include "GLSL.std.450.h"
#include "json_util.hpp"
#include "spirv_optimize.hpp"
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <filesystem>

namespace fs = std::filesystem;

// Turns one RGA CSV header line + one values line into a JSON object,
// splitting generically on commas rather than hardcoding RGA's column
// names, so this keeps working if a future RGA version adds/reorders
// columns. A value is emitted as a JSON number if it parses as one
// (every column except DEVICE is numeric today), otherwise as a string.
static std::string csv_row_to_json_object(const std::string &header, const std::string &values)
{
	auto split = [](const std::string &s)
	{
		std::vector<std::string> out;
		std::stringstream ss(s);
		std::string item;
		while (std::getline(ss, item, ','))
			out.push_back(item);
		return out;
	};
	const std::vector<std::string> keys = split(header);
	const std::vector<std::string> vals = split(values);

	std::ostringstream out;
	out << "{";
	for (size_t i = 0; i < keys.size() && i < vals.size(); ++i)
	{
		if (i > 0) out << ",";
		out << "\"" << json_escape(keys[i]) << "\":";
		char *end = nullptr;
		std::strtod(vals[i].c_str(), &end);
		const bool is_number = end != nullptr && *end == '\0' && !vals[i].empty();
		if (is_number)
			out << vals[i];
		else
			out << "\"" << json_escape(vals[i]) << "\"";
	}
	out << "}";
	return out.str();
}

struct instruction_counts
{
	size_t cheap_alu = 0;      // full/near-full rate: add, mul, mad, compare, bitwise, select
	size_t transcendental = 0; // reduced rate: sin, cos, pow, exp, log, sqrt, rsqrt, division
	size_t texture = 0;        // latency-bound: image sample/fetch/gather
	size_t control_flow = 0;   // cost scales with thread divergence: branches, loops, switches
	size_t memory = 0;         // real per-invocation cost: loads, stores, access chains, calls
	size_t declaration = 0;    // compile-time only, zero runtime cost: types, constants, decorations, capabilities
	size_t other = 0;

	// "Executed" excludes declarations, which never run at invocation time.
	size_t executed() const { return cheap_alu + transcendental + texture + control_flow + memory + other; }
	size_t total() const { return executed() + declaration; }
};

static bool is_transcendental_ext_inst(uint32_t instruction)
{
	switch (instruction)
	{
	case GLSLstd450Sin: case GLSLstd450Cos: case GLSLstd450Tan:
	case GLSLstd450Asin: case GLSLstd450Acos: case GLSLstd450Atan: case GLSLstd450Atan2:
	case GLSLstd450Sinh: case GLSLstd450Cosh: case GLSLstd450Tanh:
	case GLSLstd450Asinh: case GLSLstd450Acosh: case GLSLstd450Atanh:
	case GLSLstd450Pow: case GLSLstd450Exp: case GLSLstd450Log:
	case GLSLstd450Exp2: case GLSLstd450Log2:
	case GLSLstd450Sqrt: case GLSLstd450InverseSqrt:
		return true;
	default:
		return false;
	}
}

// Classifies every instruction in a SPIR-V module by real SPIR-V opcode.
// This is a general cross-vendor proxy (documented GPU ISA behavior), not a
// specific GPU's exact cycle timing - see --rga for hardware-accurate data.
static instruction_counts classify_spirv_instructions(const std::string &binary)
{
	instruction_counts counts;
	if (binary.size() < 20 || (binary.size() % 4) != 0)
		return counts;

	const auto *words = reinterpret_cast<const uint32_t *>(binary.data());
	const size_t word_count = binary.size() / 4;

	size_t i = 5; // skip the 5-word SPIR-V header
	while (i < word_count)
	{
		const uint32_t word_length = words[i] >> 16;
		const uint32_t opcode = words[i] & 0xFFFF;
		if (word_length == 0)
			break;

		switch (opcode)
		{
		// Cheap, (near) full-rate ALU: basic arithmetic, bitwise, data movement, compares
		case 128: case 129: // OpIAdd, OpFAdd
		case 130: case 131: // OpISub, OpFSub
		case 132: case 133: // OpIMul, OpFMul
		case 197: case 198: case 199: case 200: // OpBitwiseOr/Xor/And, OpNot
		case 194: case 195: case 196: // OpShiftRightLogical/Arithmetic, OpShiftLeftLogical
		case 169: // OpSelect
		case 170: case 171: case 172: case 173: case 174: case 175: case 176: case 177:
		case 178: case 179: case 180: case 181: case 182: case 183: case 184: case 185:
		case 186: case 187: case 188: case 189: case 190: case 191: // all compare ops
		case 79: case 80: case 81: case 82: case 83: // shuffle/construct/extract/insert/copy
		case 126: case 127: // OpSNegate, OpFNegate
			counts.cheap_alu++;
			break;

		// Division/modulo: implemented as reciprocal + multiply on most GPUs, reduced rate
		case 134: case 135: case 136: case 137: case 138: case 139: case 140: case 141:
			// OpUDiv, OpSDiv, OpFDiv, OpUMod, OpSRem, OpSMod, OpFRem, OpFMod
			counts.transcendental++;
			break;

		// Texture / image ops: latency-bound, cost hidden by occupancy rather than issue rate
		case 87: case 88: case 89: case 90: case 91: case 92: // OpImageSample* family
		case 95: case 96: case 97: // OpImageFetch/Gather/DrefGather
			counts.texture++;
			break;

		// Control flow: cost scales with thread/wave divergence, not a fixed rate
		case 246: case 247: case 249: case 250: case 251: // OpLoopMerge/SelectionMerge/Branch/BranchConditional/Switch
			counts.control_flow++;
			break;

		// Real per-invocation memory traffic and calls (not free, but not ALU either)
		case 57: // OpFunctionCall
		case 59: // OpVariable
		case 61: case 62: // OpLoad, OpStore
		case 65: case 66: // OpAccessChain, OpInBoundsAccessChain
			counts.memory++;
			break;

		// Compile-time-only declarations: zero runtime cost, never executed per-invocation
		case 1:  // OpUndef
		case 3: case 4: case 5: case 6: // OpSource, OpSourceExtension, OpName, OpMemberName
		case 11: case 14: case 15: case 16: case 17: // OpExtInstImport, OpMemoryModel, OpEntryPoint, OpExecutionMode, OpCapability
		case 19: case 20: case 21: case 22: case 23: case 24: case 25: case 26: case 27: // OpType* family
		case 28: case 29: case 30: case 32: case 33:
		case 41: case 42: case 43: case 44: case 46: // OpConstant* family
		case 54: case 55: case 56: // OpFunction, OpFunctionParameter, OpFunctionEnd
		case 71: case 72: // OpDecorate, OpMemberDecorate
		case 248: // OpLabel (structural marker, not an operation)
		case 253: case 254: // OpReturn, OpReturnValue
			counts.declaration++;
			break;

		case 12: // OpExtInst
			if (word_length >= 5 && is_transcendental_ext_inst(words[i + 4]))
				counts.transcendental++;
			else
				counts.cheap_alu++;
			break;

		default:
			counts.other++;
			break;
		}

		i += word_length;
	}
	return counts;
}

static void print_usage(const char *path)
{
	std::cout <<
		"usage: " << path << " [-D name=value] [-I path] [--rga <path-to-rga>] [--asic <name>]... [--json]\n"
		"       [--reshade-version <num>] [--perf]\n"
		"       [--load-settings[=<path>]] [--save-settings[=<path>]]\n"
		"       [--width <n>] [--height <n>] <file.fx>\n\n"
		"  -D <id>=<text>   Define a preprocessor macro. Repeatable.\n"
		"  -I <path>        Add directory to include search path. Repeatable.\n"
		"  --rga <path>     Path to the RGA (Radeon GPU Analyzer) executable.\n"
		"                   If given, real GPU ISA and register usage are fetched\n"
		"                   for each --asic. Without it, only the built-in SPIR-V\n"
		"                   instruction classification is printed.\n"
		"  --asic <name>    AMD GPU codename to target (e.g. gfx1100). Repeatable.\n"
		"                   Defaults to gfx1100 (RDNA3) if --rga is given but no\n"
		"                   --asic is specified.\n"
		"  --json           Emit machine-readable JSON instead of the default\n"
		"                   human-readable text.\n"
		"  --reshade-version <num>  Override the __RESHADE__ macro (default: the\n"
		"                   version this binary was built against).\n"
		"  --perf                   Set __RESHADE_PERFORMANCE_MODE__ to 1 (default: 0).\n"
		"  --renderer <id>  Override __RENDERER__ (default: 0x20000, Vulkan).\n"
		"                   0x9000 D3D9, 0xa000 D3D10, 0xa100 D3D10.1,\n"
		"                   0xb000 D3D11, 0xc000 D3D12, 0x10000 OpenGL.\n"
		"                   Effects that branch on __RENDERER__ compile different\n"
		"                   code per API; this is how to see which path each takes.\n"
		"  --optimize               Run the folding/inlining/mem2reg/dead-branch\n"
		"                   passes a GPU driver runs, before counting. reshadefx\n"
		"                   emits both sides of every branch and leaves all\n"
		"                   optimization to the driver, so without this the count\n"
		"                   includes code no GPU executes (LumaSharpen: 116 vs 15\n"
		"                   cheap_alu, 16 vs 5 texture). Implies --perf. This is a\n"
		"                   vendor-neutral proxy, not any specific driver - use it\n"
		"                   to compare shader versions, not to predict GPU cost.\n"
		"  --load-settings[=<path>] ReShade-preset-format file (or a real ReShade\n"
		"                   preset) to read plain-numeric uniform overrides from,\n"
		"                   applied to the source before compiling. Implies\n"
		"                   --perf, since that's the only mode where a\n"
		"                   uniform's value becomes part of the compiled code.\n"
		"                   Without =<path>, defaults to \"<effect-name>.ini\"\n"
		"                   alongside the shader.\n"
		"  --save-settings[=<path>] Scan the shader's own uniform defaults and write\n"
		"                   them out in ReShade preset format, then continue to\n"
		"                   compile as normal. Without =<path>, defaults to\n"
		"                   \"<effect-name>.ini\" alongside the shader.\n"
		"  --width <n>      Override the BUFFER_WIDTH macro (default: 1920).\n"
		"  --height <n>     Override the BUFFER_HEIGHT macro (default: 1080).\n";
}

// Baked in by the build script from RESHADE_VERSION (MAJOR*10000 + MINOR*100
// + REVISION, matching how ReShade itself computes __RESHADE__). Falls back
// to a placeholder if compiled directly without that define.
#ifndef RESHADEFX_VERSION_NUM
#define RESHADEFX_VERSION_NUM 60000
#endif

// Reads the "[effect_filename]" section of a ReShade-preset-format settings
// file (same on-disk format real ReShade uses for presets - see
// source/ini_file.cpp: one "key=value1,value2,..." line per uniform, ","
// separates components, ",," is an escaped literal comma). Only the section
// matching this shader's own filename is read; everything else (other
// effects' sections, [PRESET], techniques, etc.) is ignored, so a genuine
// multi-effect ReShade preset file can be pointed at directly.
static std::map<std::string, std::vector<std::string>> parse_settings_file(const fs::path &settings_path, const std::string &effect_filename)
{
	std::map<std::string, std::vector<std::string>> result;

	std::ifstream file(settings_path);
	if (!file)
		return result;

	const auto trim = [](std::string s) {
		const size_t begin = s.find_first_not_of(" \t");
		if (begin == std::string::npos)
			return std::string();
		const size_t end = s.find_last_not_of(" \t\r\n");
		return s.substr(begin, end - begin + 1);
	};

	std::string line;
	bool in_target_section = false;
	while (std::getline(file, line))
	{
		line = trim(line);
		if (line.empty())
			continue;

		if (line.front() == '[' && line.back() == ']')
		{
			in_target_section = (line.substr(1, line.size() - 2) == effect_filename);
			continue;
		}

		if (!in_target_section)
			continue;

		const size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;

		const std::string key = trim(line.substr(0, eq));
		const std::string value = line.substr(eq + 1);

		// Split on unescaped commas, treating ",," as one literal comma -
		// mirrors the parsing in crosire/reshade's source/ini_file.cpp.
		std::vector<std::string> components;
		std::string current;
		for (size_t i = 0; i < value.size(); ++i)
		{
			if (value[i] == ',')
			{
				if (i + 1 < value.size() && value[i + 1] == ',')
				{
					current += ',';
					++i;
				}
				else
				{
					components.push_back(trim(current));
					current.clear();
				}
			}
			else
			{
				current += value[i];
			}
		}
		components.push_back(trim(current));

		result[key] = std::move(components);
	}

	return result;
}

// The preprocessor environment the ReShade runtime sets up (runtime.cpp).
//
// This is not cosmetic. An undefined macro evaluates to 0 in #if, so a missing
// __RENDERER__ makes every renderer-conditional effect silently compile its
// fallback path with no diagnostic; and without the backwards-compatibility
// macros, any effect still using the pre-5.0 spelling of the offset/gather
// intrinsics fails outright (CAS.fx, SMAA.fx, Deband.fx in the standard packs).
static void setup_reshade_macros(reshadefx::preprocessor &pp, const std::string &reshade_version,
	bool performance_mode, const std::string &buffer_width, const std::string &buffer_height,
	unsigned int renderer_id)
{
	pp.add_macro_definition("__RESHADE__", reshade_version);
	pp.add_macro_definition("__RESHADE_PERMUTATION__", "0");
	pp.add_macro_definition("__RESHADE_PERFORMANCE_MODE__", performance_mode ? "1" : "0");
	pp.add_macro_definition("__VENDOR__", "0");
	pp.add_macro_definition("__DEVICE__", "0");
	pp.add_macro_definition("__RENDERER__", std::to_string(renderer_id));
	pp.add_macro_definition("__APPLICATION__", "0");
	pp.add_macro_definition("BUFFER_WIDTH", buffer_width);
	pp.add_macro_definition("BUFFER_HEIGHT", buffer_height);
	pp.add_macro_definition("BUFFER_RCP_WIDTH", "(1.0 / BUFFER_WIDTH)");
	pp.add_macro_definition("BUFFER_RCP_HEIGHT", "(1.0 / BUFFER_HEIGHT)");
	pp.add_macro_definition("BUFFER_COLOR_SPACE", "1");
	pp.add_macro_definition("BUFFER_COLOR_FORMAT", "28");
	pp.add_macro_definition("BUFFER_COLOR_BIT_DEPTH", "8");

	// Identical to the block runtime.cpp injects before the effect source.
	pp.append_string(
		"#define tex2Doffset(s, coords, offset) tex2D(s, coords, offset)\n"
		"#define tex2Dlodoffset(s, coords, offset) tex2Dlod(s, coords, offset)\n"
		"#define tex2Dgather(s, t, c) tex2Dgather##c(s, t)\n"
		"#define tex2Dgatheroffset(s, t, o, c) tex2Dgather##c(s, t, o)\n"
		"#define tex2Dgather0 tex2DgatherR\n"
		"#define tex2Dgather1 tex2DgatherG\n"
		"#define tex2Dgather2 tex2DgatherB\n"
		"#define tex2Dgather3 tex2DgatherA\n");
}

// Replaces the plain-number default value of matching "uniform <type> <name>
// ... = <value>;" declarations with values from a settings file, directly in
// the source text, before it is handed to the preprocessor/parser.
//
// This is necessary rather than cosmetic: reshadefx's SPIR-V backend bakes a
// uniform's default straight into the emitted OpSpecConstant literal at parse
// time (effect_codegen_spirv.cpp, define_uniform()) and never reads it back
// afterward, so mutating codegen->module().spec_constants after the fact (the
// way ReShade's own runtime.cpp does for its preset system) has no effect on
// the SPIR-V this tool emits - real ReShade instead overrides the value via
// the graphics API's specialization-constant mechanism at pipeline-creation
// time, which is downstream of anything this tool touches. Rewriting the
// literal in the source before parsing is the only way to actually exercise
// a different performance-mode code path here.
//
// Only handles plain numeric literals (matching ReShade's own preset format,
// which cannot represent arbitrary expressions either) - annotation blocks
// ("< ... >") are skipped over structurally so a ";" or ">" inside a quoted
// annotation string does not confuse the scan, but the default-value
// expression itself is located purely by bracket/quote depth, not parsed.
static std::string apply_settings_to_source(std::string source, const std::map<std::string, std::vector<std::string>> &settings)
{
	if (settings.empty())
		return source;

	const auto skip_ws = [&](size_t p) {
		while (p < source.size() && std::isspace(static_cast<unsigned char>(source[p])))
			++p;
		return p;
	};
	const auto read_identifier = [&](size_t p) {
		const size_t start = p;
		while (p < source.size() && (std::isalnum(static_cast<unsigned char>(source[p])) || source[p] == '_'))
			++p;
		return source.substr(start, p - start);
	};
	// Advances 'p' past a string literal (if one starts there) or one
	// character otherwise; used to keep quote contents from confusing the
	// bracket/paren depth tracking below.
	const auto skip_string_or_char = [&](size_t p) {
		if (p < source.size() && source[p] == '"')
		{
			++p;
			while (p < source.size() && source[p] != '"')
				p += (source[p] == '\\' && p + 1 < source.size()) ? 2 : 1;
			if (p < source.size())
				++p; // closing quote
			return p;
		}
		return p + 1;
	};

	size_t pos = 0;
	while ((pos = source.find("uniform", pos)) != std::string::npos)
	{
		size_t i = pos + 7; // length of "uniform"

		// Require a word boundary so this doesn't match part of a longer
		// identifier (e.g. a variable literally named "uniformity").
		if (i >= source.size() || !std::isspace(static_cast<unsigned char>(source[i])))
		{
			pos += 7;
			continue;
		}

		i = skip_ws(i);
		const std::string type_name = read_identifier(i);
		i += type_name.size();
		i = skip_ws(i);
		const std::string var_name = read_identifier(i);
		i += var_name.size();

		if (type_name.empty() || var_name.empty())
		{
			pos += 7;
			continue;
		}

		const auto settings_it = settings.find(var_name);

		// Skip an optional array size "[N]".
		i = skip_ws(i);
		if (i < source.size() && source[i] == '[')
		{
			const size_t close = source.find(']', i);
			if (close == std::string::npos)
				break; // malformed source - stop rather than misparse further
			i = close + 1;
		}

		// Skip an optional annotation block "< ... >", tracking nested
		// angle brackets and quoted strings so a ">" or ";" inside e.g.
		// ui_tooltip = "a; b > c"; does not end the block early.
		i = skip_ws(i);
		if (i < source.size() && source[i] == '<')
		{
			int depth = 0;
			do
			{
				if (source[i] == '"')
					i = skip_string_or_char(i);
				else
				{
					if (source[i] == '<')
						++depth;
					else if (source[i] == '>')
						--depth;
					++i;
				}
			} while (i < source.size() && depth > 0);
		}

		i = skip_ws(i);

		if (i < source.size() && source[i] == '=' && settings_it != settings.end())
		{
			const size_t value_start = i + 1;

			// Find the terminating top-level ";" - skipping over parens
			// (vector/matrix constructor syntax, e.g. "float3(1, 2, 3)")
			// and quoted strings so nothing inside those ends the scan early.
			size_t j = value_start;
			int paren_depth = 0;
			while (j < source.size())
			{
				if (source[j] == '"')
					j = skip_string_or_char(j);
				else if (source[j] == '(')
					{ ++paren_depth; ++j; }
				else if (source[j] == ')')
					{ --paren_depth; ++j; }
				else if (source[j] == ';' && paren_depth == 0)
					break;
				else
					++j;
			}

			if (j < source.size()) // found the terminating ';'
			{
				const std::vector<std::string> &values = settings_it->second;
				std::string replacement;
				if (values.size() == 1)
				{
					replacement = values[0];
				}
				else
				{
					replacement = type_name + "(";
					for (size_t k = 0; k < values.size(); ++k)
					{
						if (k != 0)
							replacement += ", ";
						replacement += values[k];
					}
					replacement += ")";
				}

				source.replace(value_start, j - value_start, replacement);
				i = value_start + replacement.size();
			}
			else
			{
				i = j;
			}
		}

		pos = i;
	}

	return source;
}

int main(int argc, char *argv[])
{
	reshadefx::preprocessor pp;

	// Defaults for the four ReShade-specific macros a shader might branch on.
	// Applied AFTER argument parsing below (mirroring how crosire's own
	// fxc.cpp does this) - add_macro_definition() silently keeps whichever
	// value was added FIRST for a given name, so a user override must reach
	// the preprocessor before these defaults do, not after.
	std::string reshade_version = std::to_string(RESHADEFX_VERSION_NUM);
	bool performance_mode = false;
	bool optimize = false;
	std::string buffer_width = "1920";
	std::string buffer_height = "1080";
	// This tool emits SPIR-V, which is what ReShade uses on Vulkan, so that is
	// the default. Override it to audit which path an effect takes on another
	// API -- several effects branch on __RENDERER__ and compile different code.
	unsigned int renderer_id = 0x20000; // Vulkan

	const char *source_file = nullptr;
	std::string rga_path;
	std::vector<std::string> asics;
	bool json_output = false;
	bool load_settings_requested = false;
	std::string settings_load_path;
	bool save_settings_requested = false;
	std::string settings_save_path;

	// -D/-I are collected here rather than applied straight to `pp`, because
	// --save-settings runs its own independent scan-only preprocess/parse
	// pass (see below) that needs the same user-supplied defines/include
	// paths as the main compile.
	std::vector<std::pair<std::string, std::string>> user_defines;
	std::vector<std::string> user_include_paths;

	for (int i = 1; i < argc; ++i)
	{
		const std::string arg = argv[i];
		if ((arg == "-h" || arg == "--help"))
		{
			print_usage(argv[0]);
			return 0;
		}
		else if (arg == "-D" && i + 1 < argc)
		{
			std::string def = argv[++i];
			const size_t eq = def.find('=');
			if (eq != std::string::npos)
				user_defines.emplace_back(def.substr(0, eq), def.substr(eq + 1));
			else
				user_defines.emplace_back(def, "1");
		}
		else if (arg == "-I" && i + 1 < argc)
		{
			user_include_paths.push_back(argv[++i]);
		}
		else if (arg == "--rga" && i + 1 < argc)
		{
			rga_path = argv[++i];
		}
		else if (arg == "--asic" && i + 1 < argc)
		{
			asics.push_back(argv[++i]);
		}
		else if (arg == "--json")
		{
			json_output = true;
		}
		else if (arg == "--reshade-version" && i + 1 < argc)
		{
			reshade_version = argv[++i];
		}
		else if (arg == "--renderer" && i + 1 < argc)
		{
			renderer_id = static_cast<unsigned int>(std::strtoul(argv[++i], nullptr, 0));
		}
		else if (arg == "--optimize")
		{
			optimize = true;
		}
		else if (arg == "--perf")
		{
			performance_mode = true;
		}
		// "--load-settings"/"--save-settings" alone use the default
		// "<effect-stem>.ini" path (filled in below, once source_file is
		// known); "=<path>" gives an explicit one. Plain space-separated
		// "--load-settings <path>" isn't supported for these two, since an
		// optional value can't be told apart from the positional <file.fx>
		// argument that always follows it.
		else if (arg == "--load-settings")
		{
			load_settings_requested = true;
		}
		else if (arg.rfind("--load-settings=", 0) == 0)
		{
			load_settings_requested = true;
			settings_load_path = arg.substr(std::strlen("--load-settings="));
		}
		else if (arg == "--save-settings")
		{
			save_settings_requested = true;
		}
		else if (arg.rfind("--save-settings=", 0) == 0)
		{
			save_settings_requested = true;
			settings_save_path = arg.substr(std::strlen("--save-settings="));
		}
		else if (arg == "--width" && i + 1 < argc)
		{
			buffer_width = argv[++i];
		}
		else if (arg == "--height" && i + 1 < argc)
		{
			buffer_height = argv[++i];
		}
		else
		{
			source_file = argv[i];
		}
	}

	if (source_file == nullptr)
	{
		print_usage(argv[0]);
		return 1;
	}
	if (!rga_path.empty() && asics.empty())
		asics.push_back("gfx1100");

	const std::string effect_filename = fs::path(source_file).filename().u8string();
	// "levels.fx" -> "levels.ini" alongside it, when no explicit path was
	// given to --load-settings/--save-settings.
	const std::string default_settings_path = (fs::path(source_file).parent_path() / (fs::path(source_file).stem().u8string() + ".ini")).u8string();
	if (load_settings_requested && settings_load_path.empty())
		settings_load_path = default_settings_path;
	if (save_settings_requested && settings_save_path.empty())
		settings_save_path = default_settings_path;

	// A settings file only has any effect at all in performance mode (that's
	// the only mode where a uniform's value becomes part of the compiled
	// code rather than a runtime-editable buffer entry) - so using one
	// implies performance mode rather than requiring both flags.
	if (load_settings_requested)
		performance_mode = true;

	// Apply defaults now, after parsing - see comment above main() for why
	// this order matters (a redefinition with a different value is silently
	// rejected, so any user override must be added to `pp` first).
	for (const auto &[name, value] : user_defines)
		pp.add_macro_definition(name, value);
	for (const std::string &path : user_include_paths)
		pp.add_include_path(path);
	setup_reshade_macros(pp, reshade_version, performance_mode,
		buffer_width, buffer_height, renderer_id);

	// Read the source into memory ourselves whenever either settings flag is
	// in play - --load-settings needs to patch it before preprocessing, and
	// --save-settings needs the untouched original to scan.
	std::string original_source_text;
	if (load_settings_requested || save_settings_requested)
	{
		std::ifstream source_stream(source_file, std::ios::binary);
		if (!source_stream)
		{
			if (json_output)
				std::cout << "{\"file\":\"" << json_escape(source_file) << "\",\"success\":false,"
					<< "\"stage\":\"read\",\"error\":\"could not open file\"}\n";
			else
				std::cout << "error: could not open " << source_file << std::endl;
			return 1;
		}
		std::ostringstream source_buffer;
		source_buffer << source_stream.rdbuf();
		original_source_text = source_buffer.str();
	}

	if (save_settings_requested)
	{
		// Independent scan-only preprocess+parse pass: same user -D/-I and
		// the four ReShade macros as the main compile (so #if-guarded
		// uniforms resolve the same way), but always with
		// uniforms_to_spec_constants=false, since module().uniforms is only
		// populated on that path (effect_codegen_spirv.cpp, define_uniform())
		// - with it on, defaults live in module().spec_constants instead.
		// Always run against the ORIGINAL source text, never anything
		// --load-settings would have patched, so this reflects the shader's
		// own authored defaults, not values we just injected.
		reshadefx::preprocessor scan_pp;
		for (const auto &[name, value] : user_defines)
			scan_pp.add_macro_definition(name, value);
		for (const std::string &path : user_include_paths)
			scan_pp.add_include_path(path);
		setup_reshade_macros(scan_pp, reshade_version, performance_mode,
			buffer_width, buffer_height, renderer_id);

		if (!scan_pp.append_string(original_source_text, source_file))
		{
			if (json_output)
				std::cout << "{\"file\":\"" << json_escape(source_file) << "\",\"success\":false,"
					<< "\"stage\":\"preprocess\",\"error\":\"" << json_escape(scan_pp.errors()) << "\"}\n";
			else
				std::cout << scan_pp.errors() << std::endl;
			return 1;
		}

		std::unique_ptr<reshadefx::codegen> scan_backend(
			reshadefx::create_codegen_spirv(false, false, false, false));

		reshadefx::parser scan_parser;
		if (!scan_parser.parse(scan_pp.output(), scan_backend.get()))
		{
			if (json_output)
				std::cout << "{\"file\":\"" << json_escape(source_file) << "\",\"success\":false,"
					<< "\"stage\":\"parse\",\"error\":\"" << json_escape(scan_pp.errors() + scan_parser.errors()) << "\"}\n";
			else
				std::cout << scan_pp.errors() << scan_parser.errors() << std::endl;
			return 1;
		}

		std::ofstream settings_out(settings_save_path, std::ios::trunc);
		if (!settings_out)
		{
			if (json_output)
				std::cout << "{\"file\":\"" << json_escape(source_file) << "\",\"success\":false,"
					<< "\"stage\":\"save-settings\",\"error\":\"could not write " << json_escape(settings_save_path) << "\"}\n";
			else
				std::cout << "error: could not write " << settings_save_path << std::endl;
			return 1;
		}

		settings_out << "[" << effect_filename << "]\n";
		for (const reshadefx::uniform &u : scan_backend->module().uniforms)
		{
			if (!u.has_initializer_value)
				continue; // nothing to write without an authored default

			settings_out << u.name << "=";
			const unsigned int component_count = std::max(1u, u.type.components());
			for (unsigned int c = 0; c < component_count; ++c)
			{
				if (c != 0)
					settings_out << ",";
				switch (u.type.base)
				{
				case reshadefx::type::t_bool:
				case reshadefx::type::t_uint:
					settings_out << u.initializer_value.as_uint[c];
					break;
				case reshadefx::type::t_int:
					settings_out << u.initializer_value.as_int[c];
					break;
				default: // t_float and the other floating-point variants
					settings_out << std::to_string(u.initializer_value.as_float[c]);
					break;
				}
			}
			settings_out << "\n";
		}

		if (!json_output)
			std::cout << "SAVED_SETTINGS " << settings_save_path << "\n";
	}

	bool preprocess_ok;
	if (load_settings_requested)
	{
		const std::map<std::string, std::vector<std::string>> settings = parse_settings_file(settings_load_path, effect_filename);
		std::string source_text = apply_settings_to_source(original_source_text, settings);

		// append_string() with a path behaves identically to append_file()
		// for include resolution etc. - append_file() itself just reads the
		// file and forwards to append_string() (effect_preprocessor.cpp).
		preprocess_ok = pp.append_string(std::move(source_text), source_file);
	}
	else
	{
		preprocess_ok = pp.append_file(source_file);
	}

	if (!preprocess_ok)
	{
		if (json_output)
			std::cout << "{\"file\":\"" << json_escape(source_file) << "\",\"success\":false,"
				<< "\"stage\":\"preprocess\",\"error\":\"" << json_escape(pp.errors()) << "\"}\n";
		else
			std::cout << pp.errors() << std::endl;
		return 1;
	}

	// --optimize without --perf would fold nothing: the uniforms stay uniforms,
	// so no branch becomes dead. Imply it rather than silently doing no work.
	if (optimize)
		performance_mode = true;

	std::unique_ptr<reshadefx::codegen> backend(
		reshadefx::create_codegen_spirv(false, false, performance_mode, false));

	reshadefx::parser parser;
	if (!parser.parse(pp.output(), backend.get()))
	{
		if (json_output)
			std::cout << "{\"file\":\"" << json_escape(source_file) << "\",\"success\":false,"
				<< "\"stage\":\"parse\",\"error\":\"" << json_escape(pp.errors() + parser.errors()) << "\"}\n";
		else
			std::cout << pp.errors() << parser.errors() << std::endl;
		return 1;
	}

	if (!json_output)
		std::cout << "COMPILE_OK " << source_file << "\n\n";

	fs::path temp_dir;
	if (!rga_path.empty())
	{
		temp_dir = fs::temp_directory_path() / "reshadefx_rga_work";
		fs::create_directories(temp_dir);
	}

	// Buffered so JSON mode can emit one clean object at the end; text mode
	// still prints incrementally below, unchanged from before.
	std::ostringstream json_entries;
	bool first_entry = true;

	for (const auto &[entry_name, stage] : backend->module().entry_points)
	{
		std::string binary, assembly, errors;
		if (!backend->assemble_code_for_entry_point(entry_name, binary, assembly, errors))
		{
			if (json_output)
			{
				if (!first_entry) json_entries << ",";
				first_entry = false;
				json_entries << "{\"name\":\"" << json_escape(entry_name) << "\",\"failed\":true,"
					<< "\"error\":\"" << json_escape(errors) << "\"}";
			}
			else
			{
				std::cout << "ENTRY_FAIL " << entry_name << " " << errors << "\n";
			}
			continue;
		}

		const char *stage_name =
			stage == reshadefx::shader_type::vertex ? "vertex" :
			stage == reshadefx::shader_type::pixel ? "pixel" :
			stage == reshadefx::shader_type::compute ? "compute" : "unknown";

		// Counting the raw module counts code no GPU executes: reshadefx emits
		// both sides of every branch and leaves optimization to the driver.
		const instruction_counts raw_counts = classify_spirv_instructions(binary);
		instruction_counts counts = raw_counts;
		bool optimized_ok = false;
		bool inliner_incomplete = false;

		if (optimize)
		{
			std::string optimized, opt_error;
			if (reshadefx_tools::spirv_optimize(binary, optimized, inliner_incomplete, opt_error))
			{
				counts = classify_spirv_instructions(optimized);
				optimized_ok = true;
				binary = optimized;
			}
			else
			{
				std::cerr << "warning: " << entry_name << ": " << opt_error << "\n";
			}
		}

		if (!json_output)
		{
			std::cout << "=== " << stage_name << " " << entry_name << " ===\n";
			std::cout << "  executed_instructions=" << counts.executed()
				<< " (+" << counts.declaration << " compile-time declarations, not counted)\n";
			std::cout << "  cheap_alu=" << counts.cheap_alu
				<< " transcendental=" << counts.transcendental
				<< " texture=" << counts.texture
				<< " control_flow=" << counts.control_flow
				<< " memory=" << counts.memory
				<< " other=" << counts.other << "\n";

			if (optimized_ok)
			{
				std::cout << "  before optimization: executed_instructions=" << raw_counts.executed()
					<< " cheap_alu=" << raw_counts.cheap_alu
					<< " texture=" << raw_counts.texture
					<< " control_flow=" << raw_counts.control_flow << "\n";
				if (inliner_incomplete)
					std::cout << "  WARNING: the SPIR-V inliner declined this module, so mem2reg could\n"
					             "           not run and the load/store scaffolding is still counted.\n"
					             "           These numbers are inflated - do not compare them.\n";
			}
		}

		std::ostringstream rga_results_json;
		bool first_rga = true;

		if (!rga_path.empty())
		{
			const fs::path spv_path = temp_dir / (entry_name + ".spv");
			{
				std::ofstream ofs(spv_path, std::ios::binary);
				ofs.write(binary.data(), binary.size());
			}

			const char *stage_flag =
				stage == reshadefx::shader_type::vertex ? "--vert" :
				stage == reshadefx::shader_type::pixel ? "--frag" :
				stage == reshadefx::shader_type::compute ? "--comp" : nullptr;

			for (const std::string &asic : (stage_flag != nullptr ? asics : std::vector<std::string>{}))
			{
				const fs::path isa_path = temp_dir / (entry_name + "_" + asic + "_isa.txt");
				const fs::path stats_path = temp_dir / (entry_name + "_" + asic + "_stats.csv");

				std::ostringstream cmd;
#ifdef _WIN32
				// cmd.exe has a well-known quirk: if the command line starts with a
				// quoted path, it must be wrapped in one more pair of quotes or it
				// mis-parses the first token. See MS KB "cmd /c" quoting behavior.
				cmd << "\"";
#endif
				cmd << "\"" << rga_path << "\" -s vulkan -c " << asic
					<< " " << stage_flag << " \"" << spv_path.string() << "\""
					<< " --isa \"" << isa_path.string() << "\""
					<< " -a \"" << stats_path.string() << "\""
					<< " > \"" << (temp_dir / "rga_log.txt").string() << "\" 2>&1";
#ifdef _WIN32
				cmd << "\"";
#endif
				std::system(cmd.str().c_str());

				// RGA renames output files (<asic>_<requested-name>_<stage>.csv) rather than
				// using the exact path given via -a, so search for what it actually produced.
				const std::string stage_suffix =
					stage == reshadefx::shader_type::vertex ? "_vert.csv" :
					stage == reshadefx::shader_type::pixel ? "_frag.csv" : "_comp.csv";
				fs::path actual_stats;
				for (const auto &dirent : fs::directory_iterator(temp_dir))
				{
					const std::string name = dirent.path().filename().string();
					if (name.find(entry_name) != std::string::npos &&
						name.find("stats") != std::string::npos &&
						name.size() >= stage_suffix.size() &&
						name.compare(name.size() - stage_suffix.size(), stage_suffix.size(), stage_suffix) == 0)
					{
						actual_stats = dirent.path();
						break;
					}
				}

				std::ifstream stats_in(actual_stats);
				if (!stats_in)
				{
					if (json_output)
					{
						if (!first_rga) rga_results_json << ",";
						first_rga = false;
						rga_results_json << "{\"asic\":\"" << json_escape(asic) << "\",\"found\":false}";
					}
					else
					{
						std::cout << "  [" << asic << "] RGA output not found (is the --rga path correct, and "
							"does this RGA build support this stage/target?)\n";
					}
					continue;
				}
				std::string header, values;
				std::getline(stats_in, header);
				std::getline(stats_in, values);

				if (json_output)
				{
					if (!first_rga) rga_results_json << ",";
					first_rga = false;
					rga_results_json << "{\"asic\":\"" << json_escape(asic) << "\",\"found\":true,"
						<< "\"stats\":" << csv_row_to_json_object(header, values) << "}";
				}
				else
				{
					std::cout << "  [" << asic << "] " << header << "\n              " << values << "\n";
				}
			}
		}

		if (json_output)
		{
			if (!first_entry) json_entries << ",";
			first_entry = false;
			json_entries << "{\"stage\":\"" << stage_name << "\",\"name\":\"" << json_escape(entry_name) << "\","
				<< "\"executed_instructions\":" << counts.executed() << ","
				<< "\"declarations\":" << counts.declaration << ","
				<< "\"cheap_alu\":" << counts.cheap_alu << ","
				<< "\"transcendental\":" << counts.transcendental << ","
				<< "\"texture\":" << counts.texture << ","
				<< "\"control_flow\":" << counts.control_flow << ","
				<< "\"memory\":" << counts.memory << ","
				<< "\"other\":" << counts.other << ","
				<< "\"rga\":[" << rga_results_json.str() << "]}";
		}
	}

	if (json_output)
	{
		std::cout << "{\"file\":\"" << json_escape(source_file) << "\",\"success\":true,"
			<< "\"entries\":[" << json_entries.str() << "]}\n";
	}

	return 0;
}

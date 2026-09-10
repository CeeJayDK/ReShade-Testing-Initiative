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
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
		"       [--reshade-version <num>] [--performance-mode] [--width <n>] [--height <n>] <file.fx>\n\n"
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
		"  --performance-mode       Set __RESHADE_PERFORMANCE_MODE__ to 1 (default: 0).\n"
		"  --width <n>      Override the BUFFER_WIDTH macro (default: 1920).\n"
		"  --height <n>     Override the BUFFER_HEIGHT macro (default: 1080).\n";
}

// Baked in by the build script from RESHADE_VERSION (MAJOR*10000 + MINOR*100
// + REVISION, matching how ReShade itself computes __RESHADE__). Falls back
// to a placeholder if compiled directly without that define.
#ifndef RESHADEFX_VERSION_NUM
#define RESHADEFX_VERSION_NUM 60000
#endif

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
	std::string buffer_width = "1920";
	std::string buffer_height = "1080";

	const char *source_file = nullptr;
	std::string rga_path;
	std::vector<std::string> asics;
	bool json_output = false;

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
				pp.add_macro_definition(def.substr(0, eq), def.substr(eq + 1));
			else
				pp.add_macro_definition(def, "1");
		}
		else if (arg == "-I" && i + 1 < argc)
		{
			pp.add_include_path(argv[++i]);
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
		else if (arg == "--performance-mode")
		{
			performance_mode = true;
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

	// Apply defaults now, after parsing - see comment above main() for why
	// this order matters (a redefinition with a different value is silently
	// rejected, so any user override must be added to `pp` first).
	pp.add_macro_definition("__RESHADE__", reshade_version);
	pp.add_macro_definition("__RESHADE_PERFORMANCE_MODE__", performance_mode ? "1" : "0");
	pp.add_macro_definition("BUFFER_WIDTH", buffer_width);
	pp.add_macro_definition("BUFFER_HEIGHT", buffer_height);
	pp.add_macro_definition("BUFFER_RCP_WIDTH", "(1.0 / BUFFER_WIDTH)");
	pp.add_macro_definition("BUFFER_RCP_HEIGHT", "(1.0 / BUFFER_HEIGHT)");

	if (!pp.append_file(source_file))
	{
		if (json_output)
			std::cout << "{\"file\":\"" << json_escape(source_file) << "\",\"success\":false,"
				<< "\"stage\":\"preprocess\",\"error\":\"" << json_escape(pp.errors()) << "\"}\n";
		else
			std::cout << pp.errors() << std::endl;
		return 1;
	}

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

		const instruction_counts counts = classify_spirv_instructions(binary);

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

/*
 * Measure how much of a ReShade shader corpus the vkd3d-backed DXBC back end
 * can actually compile, per shader model.
 *
 * The HLSL front end in vkd3d-shader is not a D3DCompiler clone, so "it built"
 * is not the same as "it works for real shaders". This walks every effect,
 * every entry point, every requested shader model, and reports exactly what
 * fails and why.
 */

#include "effect_parser.hpp"
#include "effect_codegen.hpp"
#include "effect_preprocessor.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <filesystem>
#include <algorithm>

namespace
{
	struct result
	{
		int entry_points = 0;
		int compiled = 0;
		std::vector<std::string> failures;
	};

	void setup_preprocessor(reshadefx::preprocessor &pp, unsigned int renderer_id, bool perf)
	{
		pp.add_macro_definition("__RESHADE__", "60800");
		pp.add_macro_definition("__RESHADE_PERMUTATION__", "0");
		pp.add_macro_definition("__RESHADE_PERFORMANCE_MODE__", perf ? "1" : "0");
		pp.add_macro_definition("__VENDOR__", "0");
		pp.add_macro_definition("__DEVICE__", "0");
		pp.add_macro_definition("__RENDERER__", std::to_string(renderer_id));
		pp.add_macro_definition("__APPLICATION__", "0");
		pp.add_macro_definition("BUFFER_WIDTH", "1920");
		pp.add_macro_definition("BUFFER_HEIGHT", "1080");
		pp.add_macro_definition("BUFFER_RCP_WIDTH", "(1.0 / BUFFER_WIDTH)");
		pp.add_macro_definition("BUFFER_RCP_HEIGHT", "(1.0 / BUFFER_HEIGHT)");
		pp.add_macro_definition("BUFFER_COLOR_SPACE", "1");
		pp.add_macro_definition("BUFFER_COLOR_FORMAT", "28");
		pp.add_macro_definition("BUFFER_COLOR_BIT_DEPTH", "8");
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

	unsigned int renderer_for(unsigned int sm)
	{
		if (sm >= 51) return 0xc000;
		if (sm >= 50) return 0xb000;
		if (sm >= 41) return 0xa100;
		if (sm >= 40) return 0xa000;
		return 0x9000;
	}

	// Trim a compiler message down to one informative line.
	std::string first_line(const std::string &s)
	{
		std::string line = s.substr(0, s.find('\n'));
		if (line.size() > 120)
			line.resize(120);
		return line;
	}
}

int main(int argc, char *argv[])
{
	std::vector<std::string> files;
	std::vector<std::string> includes;
	std::vector<unsigned int> models;
	bool perf = false;
	bool verbose = false;

	for (int i = 1; i < argc; ++i)
	{
		const std::string arg = argv[i];
		if (arg == "-I")                        includes.push_back(argv[++i]);
		else if (arg == "--sm")                 models.push_back(std::stoul(argv[++i]));
		else if (arg == "--spec-constants")     perf = true;
		else if (arg == "-v")                   verbose = true;
		else                                    files.push_back(arg);
	}
	if (models.empty())
		models = { 50 };
	if (files.empty())
	{
		std::fprintf(stderr, "usage: coverage [-I dir] [--sm N] [--spec-constants] [-v] <effect.fx>...\n");
		return 1;
	}

	for (const unsigned int sm : models)
	{
		std::map<std::string, result> results;
		int total_eps = 0, total_ok = 0, effects_ok = 0, effects_failed = 0;

		for (const std::string &file : files)
		{
			result r;

			reshadefx::preprocessor pp;
			setup_preprocessor(pp, renderer_for(sm), perf);
			for (const std::string &inc : includes)
				pp.add_include_path(std::filesystem::u8path(inc));
			pp.add_include_path(std::filesystem::u8path(file).parent_path());

			if (!pp.append_file(std::filesystem::u8path(file)))
			{
				r.failures.push_back("preprocessor: " + first_line(pp.errors()));
				results[file] = r;
				effects_failed++;
				continue;
			}

			std::unique_ptr<reshadefx::codegen> codegen(
				reshadefx::create_codegen_dxbc(sm, false, perf, 3));

			reshadefx::parser parser;
			if (!parser.parse(pp.output(), codegen.get()))
			{
				r.failures.push_back("reshadefx: " + first_line(parser.errors()));
				results[file] = r;
				effects_failed++;
				continue;
			}

			codegen->finalize_code();

			bool any_failed = false;
			for (const auto &entry_point : codegen->module().entry_points)
			{
				r.entry_points++;
				total_eps++;

				std::string cso, assembly, errors;
				if (codegen->assemble_code_for_entry_point(entry_point.first, cso, assembly, errors) && !cso.empty())
				{
					r.compiled++;
					total_ok++;
				}
				else
				{
					any_failed = true;
					r.failures.push_back(entry_point.first + ": " + first_line(errors));
				}
			}

			if (any_failed) effects_failed++; else effects_ok++;
			results[file] = r;
		}

		std::printf("\n=== shader model %u%s ===\n", sm, perf ? " (performance mode)" : "");
		std::printf("effects:      %d ok, %d failed\n", effects_ok, effects_failed);
		std::printf("entry points: %d / %d compiled to DXBC\n", total_ok, total_eps);

		for (const auto &[file, r] : results)
		{
			if (r.failures.empty())
			{
				if (verbose)
					std::printf("  ok    %-28s %d/%d\n",
						std::filesystem::u8path(file).filename().u8string().c_str(), r.compiled, r.entry_points);
				continue;
			}
			std::printf("  FAIL  %-28s %d/%d\n",
				std::filesystem::u8path(file).filename().u8string().c_str(), r.compiled, r.entry_points);
			for (const std::string &f : r.failures)
				std::printf("          %s\n", f.c_str());
		}
	}

	return 0;
}

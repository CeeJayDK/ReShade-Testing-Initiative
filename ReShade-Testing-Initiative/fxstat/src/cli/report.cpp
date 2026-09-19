/*
 * Output formatting for the CLI: the human table, the JSON report, the baseline
 * diff and the --dump files. None of this is used by the library.
 */

#include "report.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <vector>

namespace
{

const char *const k_header = "%-28s %-8s %6s %6s %6s %6s %6s %7s\n";

void print_stats_row(std::FILE *out, const char *name, const char *stage,
                     const fxstat::shader_stats &s, const char *suffix)
{
	std::fprintf(out, "%-28s %-8s %6u %6u %6u %6u %6u %7u%s\n",
		name, stage,
		s.get(fxstat::category::tex), s.get(fxstat::category::alu),
		s.get(fxstat::category::mov), s.get(fxstat::category::mem),
		s.get(fxstat::category::flow), s.total, suffix);
}

std::string json_escape(const std::string &s)
{
	std::string out;
	for (const char c : s)
	{
		if (c == '"' || c == '\\')
			out += '\\';
		out += c;
	}
	return out;
}

void print_counts_json(std::FILE *out, const fxstat::shader_stats &s)
{
	std::fprintf(out, "\"tex\": %u, \"alu\": %u, \"mov\": %u, \"mem\": %u, \"flow\": %u, \"total\": %u",
		s.get(fxstat::category::tex), s.get(fxstat::category::alu),
		s.get(fxstat::category::mov), s.get(fxstat::category::mem),
		s.get(fxstat::category::flow), s.total);
}

} // namespace

void fxstat::print_report(std::FILE *out, const std::string &source,
                          const compile_options &options, const compile_result &result, bool verbose)
{
	std::fprintf(out, "%s  [%s", source.c_str(), backend_name(options.target));
	if (options.target == backend::dxbc || options.target == backend::hlsl)
		std::fprintf(out, " sm%u", options.shader_model);
	std::fprintf(out, ", __RENDERER__=0x%x%s%s",
		result.renderer_used,
		options.performance_mode ? ", performance mode" : "",
		(options.target == backend::spirv) ? (options.optimize ? ", optimised" : ", raw") : "");
	// Which compiler produced a DXBC number decides whether it can be compared
	// with another one, so it is never left implicit.
	if (!result.dxbc_compiler_used.empty())
		std::fprintf(out, ", %s", result.dxbc_compiler_used.c_str());
	std::fprintf(out, "]\n");

	if (!result.uniform_values.empty())
	{
		std::fprintf(out, "\nuniform values baked in:\n");
		for (const auto &[name, value] : result.uniform_values)
			std::fprintf(out, "  %-28s = %s\n", name.c_str(), value.c_str());
	}

	std::fprintf(out, "\n");
	std::fprintf(out, k_header, "ENTRY POINT", "STAGE", "TEX", "ALU", "MOV", "MEM", "FLOW", "TOTAL");
	std::fprintf(out, "%s\n", std::string(82, '-').c_str());

	bool any_incomplete = false;

	for (const entry_point_result &r : result.entry_points)
	{
		print_stats_row(out, r.name.c_str(), r.stage.c_str(), r.stats,
			r.inliner_incomplete ? "   !!" : "");

		if (r.has_unoptimized)
			print_stats_row(out, "", "", r.stats_unoptimized, "   (before optimisation)");

		if (r.temp_registers != 0 || !r.profile.empty())
			std::fprintf(out, "%-28s %-8s %6s %6s %6s %6s %6s %7s   (%s, %u temp registers)\n",
				"", "", "", "", "", "", "", "", r.profile.c_str(), r.temp_registers);

		if (r.inliner_incomplete)
			any_incomplete = true;

		if (verbose)
		{
			std::vector<std::pair<std::string, uint32_t>> sorted(r.stats.by_opcode.begin(), r.stats.by_opcode.end());
			std::sort(sorted.begin(), sorted.end(),
				[](const auto &a, const auto &b) { return a.second > b.second; });
			for (const auto &[opcode, count] : sorted)
				std::fprintf(out, "      %-42s %4u\n", opcode.c_str(), count);
			std::fprintf(out, "\n");
		}
	}
	std::fprintf(out, "\n");

	if (any_incomplete)
		std::fprintf(out,
			"!! the SPIR-V inliner declined one or more of these modules, so mem2reg could not\n"
			"   run and the load/store scaffolding is still being counted. Those rows are\n"
			"   inflated -- do not compare them against anything. Use --dxbc for these.\n\n");
}

void fxstat::print_json(std::FILE *out, const std::string &source,
                        const compile_options &options, const compile_result &result)
{
	std::fprintf(out, "{\n");
	std::fprintf(out, "  \"source\": \"%s\",\n", json_escape(source).c_str());
	std::fprintf(out, "  \"backend\": \"%s\",\n",
		options.target == backend::spirv ? "spirv" :
		options.target == backend::hlsl ? "hlsl" :
		options.target == backend::glsl ? "glsl" : "dxbc");
	std::fprintf(out, "  \"shader_model\": %u,\n", options.shader_model);
	std::fprintf(out, "  \"renderer\": \"0x%x\",\n", result.renderer_used);
	std::fprintf(out, "  \"performance_mode\": %s,\n", options.performance_mode ? "true" : "false");
	std::fprintf(out, "  \"optimized\": %s,\n", options.optimize ? "true" : "false");
	std::fprintf(out, "  \"optimizer_version\": \"%s\",\n", json_escape(result.optimizer_version).c_str());
	if (!result.dxbc_compiler_used.empty())
		std::fprintf(out, "  \"dxbc_compiler\": \"%s\",\n", json_escape(result.dxbc_compiler_used).c_str());

	std::fprintf(out, "  \"uniform_values\": {");
	for (size_t i = 0; i < result.uniform_values.size(); ++i)
		std::fprintf(out, "%s\n    \"%s\": \"%s\"", i ? "," : "",
			json_escape(result.uniform_values[i].first).c_str(),
			json_escape(result.uniform_values[i].second).c_str());
	std::fprintf(out, "%s},\n", result.uniform_values.empty() ? "" : "\n  ");

	std::fprintf(out, "  \"entry_points\": [");
	for (size_t i = 0; i < result.entry_points.size(); ++i)
	{
		const entry_point_result &r = result.entry_points[i];
		std::fprintf(out, "%s\n    {\n", i ? "," : "");
		std::fprintf(out, "      \"name\": \"%s\",\n", json_escape(r.name).c_str());
		std::fprintf(out, "      \"stage\": \"%s\",\n", r.stage.c_str());
		if (!r.profile.empty())
		{
			std::fprintf(out, "      \"profile\": \"%s\",\n", r.profile.c_str());
			std::fprintf(out, "      \"temp_registers\": %u,\n", r.temp_registers);
		}
		// An inflated row must be visibly marked in machine output too, or a
		// consumer will treat it as a real measurement.
		if (r.inliner_incomplete)
			std::fprintf(out, "      \"inliner_incomplete\": true,\n");
		std::fprintf(out, "      ");
		print_counts_json(out, r.stats);
		if (r.has_unoptimized)
		{
			std::fprintf(out, ",\n      \"unoptimized\": { ");
			print_counts_json(out, r.stats_unoptimized);
			std::fprintf(out, " }");
		}
		std::fprintf(out, "\n    }");
	}
	std::fprintf(out, "\n  ]\n}\n");
}

bool fxstat::print_diff(std::FILE *out, const compile_result &result,
                        const std::map<std::string, baseline_entry> &baseline)
{
	auto delta = [](uint32_t now, uint32_t before) -> std::string {
		if (now == before)
			return ".";
		char buffer[32];
		std::snprintf(buffer, sizeof(buffer), "%+d", static_cast<int>(now) - static_cast<int>(before));
		return buffer;
	};

	bool regressed = false;

	std::fprintf(out, "\n%-28s %-8s %13s %13s %13s %13s %13s %13s\n",
		"ENTRY POINT", "STAGE", "TEX", "ALU", "MOV", "MEM", "FLOW", "TOTAL");
	std::fprintf(out, "%s\n", std::string(124, '-').c_str());

	for (const entry_point_result &r : result.entry_points)
	{
		const auto it = baseline.find(r.name);
		if (it == baseline.end())
		{
			print_stats_row(out, r.name.c_str(), r.stage.c_str(), r.stats, "   (new)");
			continue;
		}

		const baseline_entry &b = it->second;
		const uint32_t now[6]    = { r.stats.get(category::tex), r.stats.get(category::alu),
		                             r.stats.get(category::mov), r.stats.get(category::mem),
		                             r.stats.get(category::flow), r.stats.total };
		const uint32_t before[6] = { b.tex, b.alu, b.mov, b.mem, b.flow, b.total };

		std::string cells;
		for (int i = 0; i < 6; ++i)
		{
			if (now[i] > before[i])
				regressed = true;
			char buffer[40];
			std::snprintf(buffer, sizeof(buffer), " %8u %-4s", now[i], delta(now[i], before[i]).c_str());
			cells += buffer;
		}
		std::fprintf(out, "%-28s %-8s%s%s\n", r.name.c_str(), r.stage.c_str(), cells.c_str(),
			r.inliner_incomplete ? "  !!" : "");
	}
	std::fprintf(out, "\n");
	return regressed;
}

void fxstat::write_dump(const std::string &dir, const std::string &source,
                        const compile_options &options, const compile_result &result)
{
	const std::filesystem::path base = std::filesystem::u8path(dir);
	const std::string stem = std::filesystem::u8path(source).stem().u8string();

	if (!result.generated_code.empty())
	{
		const char *ext =
			options.target == backend::hlsl ? ".hlsl" :
			options.target == backend::glsl ? ".glsl" : ".txt";
		std::ofstream(base / (stem + ext)) << result.generated_code;
	}

	for (const entry_point_result &r : result.entry_points)
	{
		if (!r.binary.empty())
		{
			const char *ext = options.target == backend::dxbc ? ".cso" : ".spv";
			std::ofstream f(base / (r.name + ext), std::ios::binary);
			f.write(r.binary.data(), static_cast<std::streamsize>(r.binary.size()));
		}
		if (!r.assembly.empty())
			std::ofstream(base / (r.name + ".asm")) << r.assembly;
	}
}

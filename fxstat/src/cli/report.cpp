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
                     const fxstat::shader_stats &s, const char *suffix, bool lanes = false)
{
	std::fprintf(out, "%-28s %-8s %6u %6u %6u %6u %6u %7u",
		name, stage,
		s.get(fxstat::category::tex), s.get(fxstat::category::alu),
		s.get(fxstat::category::mov), s.get(fxstat::category::mem),
		s.get(fxstat::category::flow), s.total);
	if (lanes)
		std::fprintf(out, " %7u %6u", s.alu_lanes, s.trans_lanes);
	std::fprintf(out, "%s\n", suffix);
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

void print_counts_json(std::FILE *out, const fxstat::shader_stats &s, bool lanes)
{
	std::fprintf(out, "\"tex\": %u, \"alu\": %u, \"mov\": %u, \"mem\": %u, \"flow\": %u, \"total\": %u",
		s.get(fxstat::category::tex), s.get(fxstat::category::alu),
		s.get(fxstat::category::mov), s.get(fxstat::category::mem),
		s.get(fxstat::category::flow), s.total);
	if (lanes)
		std::fprintf(out, ", \"alu_lanes\": %u, \"trans\": %u, \"trans_lanes\": %u",
			s.alu_lanes, s.trans, s.trans_lanes);
}

void print_isa_json(std::FILE *out, const fxstat::isa_stats &i)
{
	if (!i.ok)
	{
		std::fprintf(out, "{ \"error\": \"%s\" }", json_escape(i.error).c_str());
		return;
	}
	std::fprintf(out,
		"{ \"valu\": %u, \"trans\": %u, \"salu\": %u, \"vmem\": %u, \"smem\": %u, \"scratch\": %u, "
		"\"branch\": %u, \"total\": %u, \"vgprs\": %u, \"sgprs\": %u, \"scratch_bytes\": %u, "
		"\"vgpr_spills\": %u, \"isa_bytes\": %u, \"cost\": %u }",
		i.valu, i.trans, i.salu, i.vmem, i.smem, i.scratch, i.branch, i.total,
		i.vgprs, i.sgprs, i.scratch_bytes, i.vgpr_spills, i.isa_bytes, i.cost());
}

void print_isa_table(std::FILE *out, const fxstat::compile_result &result,
                     const fxstat::isa_map &isa, const std::string &asic)
{
	std::fprintf(out, "GPU ISA via RGA (%s) -- what the driver actually runs, per invocation:\n\n", asic.c_str());
	std::fprintf(out, "%-28s %-8s %6s %6s %6s %6s %7s %6s %7s\n",
		"ENTRY POINT", "STAGE", "VALU", "TRANS", "SALU", "VMEM", "SCRATCH", "VGPRs", "COST");
	std::fprintf(out, "%s\n", std::string(88, '-').c_str());
	bool any_scratch = false;
	for (const fxstat::entry_point_result &r : result.entry_points)
	{
		const auto it = isa.find(r.name);
		if (it == isa.end())
			continue;
		const fxstat::isa_stats &i = it->second;
		if (!i.ok)
		{
			std::fprintf(out, "%-28s %-8s   %s\n", r.name.c_str(), r.stage.c_str(), i.error.c_str());
			continue;
		}
		const bool scratch = i.scratch != 0 || i.scratch_bytes != 0 || i.vgpr_spills != 0;
		any_scratch |= scratch;
		std::fprintf(out, "%-28s %-8s %6u %6u %6u %6u %7u %6u %7u%s\n",
			r.name.c_str(), r.stage.c_str(), i.valu, i.trans, i.salu, i.vmem, i.scratch, i.vgprs, i.cost(),
			scratch ? "   !! scratch" : "");
	}
	std::fprintf(out, "\n");
	std::fprintf(out,
		"VALU and TRANS are per pixel (or vertex); SALU runs once per wave. COST = VALU + 3 x TRANS,\n"
		"since transcendentals issue at quarter rate. Memory latency is not in COST: compare VMEM.\n\n");
	if (any_scratch)
		std::fprintf(out,
			"!! scratch memory in use: the driver has put data in video memory instead of registers.\n"
			"   Usually a local array indexed by a non-constant (a uniform, or a loop that could not be\n"
			"   unrolled), or register pressure spilling. Make lookup tables `static const`, give\n"
			"   loops over arrays a constant bound, or split the array use.\n\n");
}

} // namespace

void fxstat::print_report(std::FILE *out, const std::string &source,
                          const compile_options &options, const compile_result &result, bool verbose,
                          const isa_map *isa, const std::string &asic)
{
	const bool lanes = options.target == backend::spirv;
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
	if (lanes)
	{
		std::fprintf(out, "%-28s %-8s %6s %6s %6s %6s %6s %7s %7s %6s\n",
			"ENTRY POINT", "STAGE", "TEX", "ALU", "MOV", "MEM", "FLOW", "TOTAL", "LANES", "TRANS");
		std::fprintf(out, "%s\n", std::string(97, '-').c_str());
	}
	else
	{
		std::fprintf(out, k_header, "ENTRY POINT", "STAGE", "TEX", "ALU", "MOV", "MEM", "FLOW", "TOTAL");
		std::fprintf(out, "%s\n", std::string(82, '-').c_str());
	}

	bool any_incomplete = false;

	for (const entry_point_result &r : result.entry_points)
	{
		print_stats_row(out, r.name.c_str(), r.stage.c_str(), r.stats,
			r.inliner_incomplete ? "   !!" : "", lanes);

		if (r.has_unoptimized)
			print_stats_row(out, "", "", r.stats_unoptimized, "   (before optimisation)", lanes);

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

	if (lanes)
		std::fprintf(out,
			"LANES = ALU instructions weighted by vector width (a float4 op is 4 on the GPU).\n"
			"TRANS = transcendental ops (sin/cos/exp/log/sqrt/rsqrt/rcp) x width, quarter rate.\n"
			"Both are pre-driver estimates%s\n\n", isa ? "." : "; add --rga <path> for the real GPU ISA.");

	if (isa)
		print_isa_table(out, result, *isa, asic);
}

void fxstat::print_json(std::FILE *out, const std::string &source,
                        const compile_options &options, const compile_result &result,
                        const isa_map *isa, const std::string &asic)
{
	const bool lanes = options.target == backend::spirv;
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
	if (isa)
		std::fprintf(out, "  \"isa_target\": \"%s\",\n", json_escape(asic).c_str());

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
		print_counts_json(out, r.stats, lanes);
		if (r.has_unoptimized)
		{
			std::fprintf(out, ",\n      \"unoptimized\": { ");
			print_counts_json(out, r.stats_unoptimized, lanes);
			std::fprintf(out, " }");
		}
		if (isa)
		{
			if (const auto it = isa->find(r.name); it != isa->end())
			{
				std::fprintf(out, ",\n      \"isa\": ");
				print_isa_json(out, it->second);
			}
		}
		std::fprintf(out, "\n    }");
	}
	std::fprintf(out, "\n  ]\n}\n");
}

bool fxstat::print_diff(std::FILE *out, const compile_result &result,
                        const std::map<std::string, baseline_entry> &baseline,
                        const isa_map *isa)
{
	auto cell = [](uint32_t now, uint32_t before) -> std::string {
		char buffer[40];
		if (now == before)
			std::snprintf(buffer, sizeof(buffer), " %8u %-5s", now, ".");
		else
			std::snprintf(buffer, sizeof(buffer), " %8u %-+5d", now, static_cast<int>(now) - static_cast<int>(before));
		return buffer;
	};

	bool regressed = false;

	// Lane columns only when both sides have them (older baselines do not).
	bool lanes = false;
	for (const entry_point_result &r : result.entry_points)
		if (const auto it = baseline.find(r.name); it != baseline.end() && it->second.has_lanes)
			lanes = true;

	std::fprintf(out, "\n%-28s %-8s %14s %14s %14s %14s %14s %14s", "ENTRY POINT", "STAGE",
		"TEX", "ALU", "MOV", "MEM", "FLOW", "TOTAL");
	if (lanes)
		std::fprintf(out, " %14s %14s", "LANES", "TRANS");
	std::fprintf(out, "\n%s\n", std::string(lanes ? 162 : 132, '-').c_str());

	struct isa_row { std::string name, stage; const isa_stats *now; const baseline_entry *before; };
	std::vector<isa_row> isa_rows;

	for (const entry_point_result &r : result.entry_points)
	{
		const auto it = baseline.find(r.name);
		if (it == baseline.end())
		{
			print_stats_row(out, r.name.c_str(), r.stage.c_str(), r.stats, "   (new)", lanes);
			continue;
		}

		const baseline_entry &b = it->second;
		std::vector<std::pair<uint32_t, uint32_t>> cols = {
			{ r.stats.get(category::tex), b.tex }, { r.stats.get(category::alu), b.alu },
			{ r.stats.get(category::mov), b.mov }, { r.stats.get(category::mem), b.mem },
			{ r.stats.get(category::flow), b.flow }, { r.stats.total, b.total } };
		if (lanes && b.has_lanes)
		{
			cols.push_back({ r.stats.alu_lanes, b.alu_lanes });
			cols.push_back({ r.stats.trans_lanes, b.trans_lanes });
		}

		// With ISA on both sides it is the authority: pre-driver counts can go
		// up for a change that makes the real shader cheaper, and vice versa.
		const isa_stats *now_isa = nullptr;
		if (isa)
			if (const auto i = isa->find(r.name); i != isa->end() && i->second.ok)
				now_isa = &i->second;
		const bool judge_by_isa = now_isa != nullptr && b.has_isa;

		std::string cells;
		for (const auto &[now, before] : cols)
		{
			if (!judge_by_isa && now > before)
				regressed = true;
			cells += cell(now, before);
		}
		std::fprintf(out, "%-28s %-8s%s%s\n", r.name.c_str(), r.stage.c_str(), cells.c_str(),
			r.inliner_incomplete ? "  !!" : "");

		if (now_isa)
		{
			isa_rows.push_back({ r.name, r.stage, now_isa, b.has_isa ? &b : nullptr });
			if (judge_by_isa &&
			    (now_isa->cost() > b.isa_cost() || now_isa->vmem > b.isa_vmem || now_isa->scratch > b.isa_scratch))
				regressed = true;
		}
	}
	std::fprintf(out, "\n");

	if (!isa_rows.empty())
	{
		std::fprintf(out, "GPU ISA via RGA:\n\n%-28s %-8s %14s %14s %14s %14s %14s %14s %14s\n",
			"ENTRY POINT", "STAGE", "VALU", "TRANS", "SALU", "VMEM", "SCRATCH", "VGPRs", "COST");
		std::fprintf(out, "%s\n", std::string(141, '-').c_str());
		for (const isa_row &row : isa_rows)
		{
			const isa_stats &n = *row.now;
			if (row.before == nullptr)
			{
				std::fprintf(out, "%-28s %-8s %8u       %8u       %8u       %8u       %8u       %8u       %8u         (baseline has no ISA)\n",
					row.name.c_str(), row.stage.c_str(), n.valu, n.trans, n.salu, n.vmem, n.scratch, n.vgprs, n.cost());
				continue;
			}
			const baseline_entry &b = *row.before;
			std::fprintf(out, "%-28s %-8s%s%s%s%s%s%s%s\n", row.name.c_str(), row.stage.c_str(),
				cell(n.valu, b.isa_valu).c_str(), cell(n.trans, b.isa_trans).c_str(),
				cell(n.salu, b.isa_salu).c_str(), cell(n.vmem, b.isa_vmem).c_str(),
				cell(n.scratch, b.isa_scratch).c_str(), cell(n.vgprs, b.isa_vgprs).c_str(),
				cell(n.cost(), b.isa_cost()).c_str());
		}
		std::fprintf(out, "\nWhere both sides have ISA, regressions are judged on COST, VMEM and SCRATCH only.\n\n");
	}
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

#include "rga.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace
{
	bool starts_with(const std::string &s, const char *prefix)
	{
		return s.rfind(prefix, 0) == 0;
	}

	std::string read_text(const fs::path &path)
	{
		std::ifstream f(path, std::ios::binary);
		std::stringstream ss;
		ss << f.rdbuf();
		return ss.str();
	}

	std::string quote(const std::string &s)
	{
		return '"' + s + '"';
	}

	const char *stage_flag(const std::string &stage)
	{
		if (stage == "vertex")  return "--vert";
		if (stage == "pixel")   return "--frag";
		if (stage == "compute") return "--comp";
		return nullptr;
	}

	// Instructions that only schedule or wait and do no work.
	bool is_bookkeeping(const std::string &op)
	{
		return starts_with(op, "s_waitcnt") || op == "s_delay_alu" || op == "s_clause" ||
		       op == "s_nop" || op == "s_endpgm" || op == "s_sendmsg" || op == "s_inst_prefetch" ||
		       op == "s_set_inst_prefetch_distance";
	}

	bool is_transcendental(const std::string &op)
	{
		static const char *const k[] = { "v_exp_", "v_log_", "v_rcp_", "v_rsq_", "v_sqrt_", "v_sin_", "v_cos_" };
		for (const char *p : k)
			if (starts_with(op, p))
				return true;
		return false;
	}
}

fxstat::isa_stats fxstat::parse_rga_output(const std::string &isa_text, const std::string &csv_text)
{
	isa_stats s;

	std::istringstream lines(isa_text);
	std::string line;
	while (std::getline(lines, line))
	{
		// Instructions are tab-indented; labels, comments and the header are not.
		if (line.empty() || line[0] != '\t')
			continue;
		std::istringstream words(line);
		std::string op;
		words >> op;
		if (op.empty() || starts_with(op, "//") || op.back() == ':')
			continue;

		s.total++;
		if (starts_with(op, "scratch_"))
			s.scratch++;
		else if (starts_with(op, "image_") || starts_with(op, "buffer_") ||
		         starts_with(op, "global_") || starts_with(op, "tbuffer_") || starts_with(op, "flat_"))
			s.vmem++;
		else if (starts_with(op, "s_load") || starts_with(op, "s_buffer_load"))
			s.smem++;
		else if (op == "s_branch" || starts_with(op, "s_cbranch"))
			s.branch++;
		else if (starts_with(op, "s_"))
		{
			if (!is_bookkeeping(op))
				s.salu++;
		}
		else if (starts_with(op, "v_") && !starts_with(op, "v_interp"))
		{
			s.valu++;
			if (is_transcendental(op))
				s.trans++;
		}
	}

	// Resource usage: a header line and a value line, comma separated.
	std::istringstream csv(csv_text);
	std::string header, values;
	std::getline(csv, header);
	std::getline(csv, values);
	std::vector<std::string> keys, vals;
	for (std::istringstream h(header); std::getline(h, line, ',');) keys.push_back(line);
	for (std::istringstream v(values); std::getline(v, line, ',');) vals.push_back(line);
	auto field = [&](const char *name) -> uint32_t {
		for (size_t i = 0; i < keys.size() && i < vals.size(); ++i)
			if (keys[i] == name)
				return static_cast<uint32_t>(std::strtoul(vals[i].c_str(), nullptr, 10));
		return 0;
	};
	s.vgprs = field("USED_VGPRs");
	s.sgprs = field("USED_SGPRs");
	s.scratch_bytes = field("SCRATCH_MEM");
	s.vgpr_spills = field("VGPR_SPILLS");
	s.isa_bytes = field("ISA_SIZE");

	s.ok = s.total != 0;
	if (!s.ok)
		s.error = "RGA produced an empty ISA listing";
	return s;
}

std::map<std::string, fxstat::isa_stats> fxstat::run_rga(const rga_options &options, const compile_result &result)
{
	std::map<std::string, isa_stats> out;

	std::error_code ec;
	std::mt19937_64 rng(std::random_device{}());
	const fs::path dir = fs::temp_directory_path(ec) / ("fxstat-rga-" + std::to_string(rng()));
	fs::create_directories(dir, ec);
	if (ec)
	{
		isa_stats failed;
		failed.error = "could not create a temporary directory: " + ec.message();
		for (const entry_point_result &r : result.entry_points)
			out[r.name] = failed;
		return out;
	}

	for (const entry_point_result &r : result.entry_points)
	{
		isa_stats &s = out[r.name];
		const char *flag = stage_flag(r.stage);
		if (flag == nullptr || r.binary.empty())
		{
			s.error = r.binary.empty() ? "no SPIR-V binary" : "unsupported stage";
			continue;
		}

		const fs::path spv = dir / (r.name + ".spv");
		{
			std::ofstream f(spv, std::ios::binary);
			f.write(r.binary.data(), static_cast<std::streamsize>(r.binary.size()));
		}

		// RGA prefixes the output names with the ASIC and suffixes the stage,
		// e.g. <dir>/gfx1100_E__MyPass_frag.isa, so search rather than guess.
		const fs::path sub = dir / r.name;
		fs::create_directories(sub, ec);
		const fs::path log = dir / (r.name + ".log");
		std::string cmd = quote(options.executable) + " -s vk-spv-offline -c " + options.asic + ' ' + flag + ' ' +
			quote(spv.u8string()) + " --isa " + quote((sub / "out.isa").u8string()) +
			" -a " + quote((sub / "out.csv").u8string()) + " > " + quote(log.u8string()) + " 2>&1";
#ifdef _WIN32
		// cmd.exe strips the outer quotes of a command line that starts with one.
		cmd = '"' + cmd + '"';
#endif
		// The exit code is not trusted either way: success is an ISA file appearing.
		(void)!std::system(cmd.c_str());

		std::string isa_text, csv_text;
		for (const auto &entry : fs::directory_iterator(sub, ec))
		{
			const std::string ext = entry.path().extension().u8string();
			if (ext == ".isa")
				isa_text = read_text(entry.path());
			else if (ext == ".csv")
				csv_text = read_text(entry.path());
		}

		if (isa_text.empty())
		{
			std::string msg = read_text(log);
			if (msg.size() > 400)
				msg = msg.substr(msg.size() - 400);
			s.error = "RGA failed" + (msg.empty() ? std::string() : ": " + msg);
			continue;
		}
		s = parse_rga_output(isa_text, csv_text);
	}

	fs::remove_all(dir, ec);
	return out;
}

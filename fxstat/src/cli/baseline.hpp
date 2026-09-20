#pragma once

#include <cstdint>
#include <string>
#include <map>

namespace fxstat
{
	struct baseline_entry
	{
		std::string stage;
		uint32_t tex = 0, alu = 0, mov = 0, mem = 0, flow = 0, total = 0;

		// Absent from reports written before these existed.
		bool has_lanes = false;
		uint32_t alu_lanes = 0, trans_lanes = 0;

		// Present only when the baseline was written with --rga.
		bool has_isa = false;
		uint32_t isa_valu = 0, isa_trans = 0, isa_salu = 0, isa_vmem = 0, isa_scratch = 0, isa_vgprs = 0;
		uint32_t isa_cost() const { return isa_valu + 3 * isa_trans; }
	};

	// Read a report previously written by `fxstat --json`, keyed by entry point name.
	bool load_baseline(const std::string &path, std::map<std::string, baseline_entry> &out, std::string &error);
}

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
	};

	// Read a report previously written by `fxstat --json`, keyed by entry point name.
	bool load_baseline(const std::string &path, std::map<std::string, baseline_entry> &out, std::string &error);
}

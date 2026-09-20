#pragma once

#include "fxstat/fxstat.hpp"
#include "baseline.hpp"
#include "rga.hpp"

#include <cstdio>
#include <map>
#include <string>

namespace fxstat
{
	// `isa` is null unless --rga was given; `asic` names the GPU it was compiled for.
	using isa_map = std::map<std::string, isa_stats>;

	void print_report(std::FILE *out, const std::string &source,
	                  const compile_options &options, const compile_result &result, bool verbose,
	                  const isa_map *isa = nullptr, const std::string &asic = {});

	void print_json(std::FILE *out, const std::string &source,
	                const compile_options &options, const compile_result &result,
	                const isa_map *isa = nullptr, const std::string &asic = {});

	// Returns true if anything got more expensive against the baseline. Where
	// both sides have ISA statistics those decide (cost, texture fetches,
	// scratch); otherwise the instruction and lane counts do.
	bool print_diff(std::FILE *out, const compile_result &result,
	                const std::map<std::string, baseline_entry> &baseline,
	                const isa_map *isa = nullptr);

	void write_dump(const std::string &dir, const std::string &source,
	                const compile_options &options, const compile_result &result);
}

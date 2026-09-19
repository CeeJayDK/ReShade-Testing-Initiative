#pragma once

#include "fxstat/fxstat.hpp"
#include "baseline.hpp"

#include <cstdio>
#include <map>
#include <string>

namespace fxstat
{
	void print_report(std::FILE *out, const std::string &source,
	                  const compile_options &options, const compile_result &result, bool verbose);

	void print_json(std::FILE *out, const std::string &source,
	                const compile_options &options, const compile_result &result);

	// Returns true if any count went up against the baseline.
	bool print_diff(std::FILE *out, const compile_result &result,
	                const std::map<std::string, baseline_entry> &baseline);

	void write_dump(const std::string &dir, const std::string &source,
	                const compile_options &options, const compile_result &result);
}

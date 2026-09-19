#pragma once

#include <string>
#include <vector>

namespace fxstat
{
	// Run the driver-equivalent pass list over a SPIR-V module.
	bool optimize_spirv(const std::string &input, std::string &output, std::string &error);
}

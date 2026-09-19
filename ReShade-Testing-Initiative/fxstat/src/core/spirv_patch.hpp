#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fxstat
{
	// Overwrite every OpSpecConstant / OpSpecConstantTrue / OpSpecConstantFalse
	// whose SpecId decoration is in range with the corresponding raw 32-bit
	// value. Index into `values_by_spec_id` is the SpecId; the value is the bit
	// pattern (float bits for floats, the integer itself for ints, 0/non-0 for
	// bools). Patches `binary` in place.
	bool patch_spec_constants(std::string &binary,
		const std::vector<uint32_t> &values_by_spec_id, std::string &error);
}

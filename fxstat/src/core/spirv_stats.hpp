/*
 * SPIR-V instruction classifier.
 *
 * A SPIR-V binary is a flat stream of (wordcount << 16 | opcode) headers, so
 * walking it needs no dependency on SPIRV-Tools. The categories and the stats
 * struct are the public ones from fxstat.hpp.
 */
#pragma once

#include "fxstat/fxstat.hpp"

#include <cstdint>
#include <string>

namespace fxstat
{
	// Category for an opcode, and whether it is a body instruction at all
	// (types, constants, decorations and debug info are declarations).
	bool classify_opcode(uint16_t opcode, category &out);
	const char *opcode_name(uint16_t opcode);

	// Walk a SPIR-V binary and count instructions reachable from its entry
	// points. `inliner_incomplete` is set when an OpFunctionParameter survives,
	// which after the driver pass list means the inliner declined the module and
	// the counts are inflated.
	bool analyze_spirv(const uint32_t *words, size_t word_count, shader_stats &out,
	                   bool *inliner_incomplete, std::string &error);

	inline bool analyze_spirv(const std::string &binary, shader_stats &out,
	                          bool *inliner_incomplete, std::string &error)
	{
		if (binary.size() % 4 != 0)
		{
			error = "SPIR-V binary size is not a multiple of 4";
			return false;
		}
		return analyze_spirv(reinterpret_cast<const uint32_t *>(binary.data()),
		                     binary.size() / 4, out, inliner_incomplete, error);
	}
}

#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <map>

namespace fxstat
{
	enum class dxbc_category : uint8_t
	{
		tex, alu, mov, mem, flow, decl, count_
	};

	struct dxbc_stats
	{
		uint32_t counts[static_cast<size_t>(dxbc_category::count_)] = {};
		uint32_t instructions = 0;   // executed instructions, declarations excluded
		uint32_t declarations = 0;
		uint32_t temp_registers = 0; // from dcl_temps
		std::string profile;         // e.g. "ps_5_0"
		std::map<std::string, uint32_t> by_opcode;

		uint32_t get(dxbc_category c) const { return counts[static_cast<size_t>(c)]; }
	};

	dxbc_category classify_dxbc_opcode(const std::string &opcode);
	const char *dxbc_category_name(dxbc_category c);

	// Parse a d3d-asm disassembly listing (as produced by vkd3d-shader's
	// D3D_ASM target, or by D3DDisassemble) and count instructions.
	bool analyze_dxbc_asm(const std::string &listing, dxbc_stats &out, std::string &error);
}

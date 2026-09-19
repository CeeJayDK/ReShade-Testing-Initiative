/*
 * Classify DXBC / D3D-bytecode instructions from a d3d-asm disassembly listing.
 *
 * Unlike SPIR-V, DXBC is a virtual ISA with a small, well-documented and stable
 * opcode set, so classifying it from the text listing is exact rather than
 * approximate. This is the number that corresponds to what ReShade actually
 * hands a D3D driver.
 */

#include "dxbc_stats.hpp"

#include <algorithm>
#include <cctype>

namespace
{
	// An opcode may carry result modifiers (mov_sat), a result-type suffix
	// (dp4, utof) or a conditional form (if_nz, breakc_nz). Strip the modifiers
	// that do not change the class of the instruction.
	std::string base_opcode(std::string op)
	{
		static const char *const modifiers[] = { "_sat", "_pp", "_centroid" };
		for (const char *m : modifiers)
		{
			const size_t at = op.find(m);
			if (at != std::string::npos)
				op.erase(at, std::char_traits<char>::length(m));
		}
		return op;
	}

	bool starts_with(const std::string &s, const char *prefix)
	{
		return s.compare(0, std::char_traits<char>::length(prefix), prefix) == 0;
	}
}

fxstat::dxbc_category fxstat::classify_dxbc_opcode(const std::string &raw_opcode)
{
	const std::string op = base_opcode(raw_opcode);

	// Declarations describe the shader's interface; they are not executed.
	if (starts_with(op, "dcl_") || op == "dcl" ||
		starts_with(op, "def") || starts_with(op, "dq"))
		return dxbc_category::decl;

	// Texture and buffer access.
	if (starts_with(op, "sample") || starts_with(op, "gather4") ||
		starts_with(op, "ld") || starts_with(op, "texld") ||
		op == "lod" || op == "resinfo" || op == "bufinfo" ||
		op == "texkill" /* SM3 discard, but it is a texture-unit op */)
		return op == "texkill" ? dxbc_category::flow : dxbc_category::tex;

	// Control flow.
	if (starts_with(op, "if") || op == "else" || op == "endif" ||
		starts_with(op, "loop") || op == "endloop" || op == "rep" || op == "endrep" ||
		starts_with(op, "break") || starts_with(op, "continue") ||
		starts_with(op, "ret") || op == "discard" || starts_with(op, "call") ||
		starts_with(op, "switch") || op == "case" || op == "default" || op == "endswitch" ||
		op == "label" || op == "sync" || op == "emit" || op == "cut" || op == "emitthencut")
		return dxbc_category::flow;

	// Plain moves. movc is a select and counts as arithmetic.
	if (op == "mov")
		return dxbc_category::mov;

	// Memory / atomics / UAV traffic.
	if (starts_with(op, "store") || starts_with(op, "atomic") ||
		starts_with(op, "imm_atomic"))
		return dxbc_category::mem;

	// Anything else that executes is arithmetic. The DXBC opcode set is small
	// enough that this is a safe default, but count what lands here so an
	// unfamiliar opcode shows up in --verbose rather than hiding.
	return dxbc_category::alu;
}

const char *fxstat::dxbc_category_name(dxbc_category c)
{
	switch (c)
	{
	case dxbc_category::tex:  return "TEX";
	case dxbc_category::alu:  return "ALU";
	case dxbc_category::mov:  return "MOV";
	case dxbc_category::mem:  return "MEM";
	case dxbc_category::flow: return "FLOW";
	default:                  return "DECL";
	}
}

bool fxstat::analyze_dxbc_asm(const std::string &listing, dxbc_stats &out, std::string &error)
{
	if (listing.empty())
	{
		error = "empty disassembly listing";
		return false;
	}

	size_t line_start = 0;
	bool first = true;

	while (line_start < listing.size())
	{
		size_t line_end = listing.find('\n', line_start);
		if (line_end == std::string::npos)
			line_end = listing.size();

		std::string line = listing.substr(line_start, line_end - line_start);
		line_start = line_end + 1;

		// Trim leading whitespace (nested blocks are indented).
		const size_t begin = line.find_first_not_of(" \t\r");
		if (begin == std::string::npos)
			continue;
		line = line.substr(begin);

		if (line.empty() || line[0] == '/' || line[0] == '#')
			continue;

		// The first non-empty line is the profile, e.g. "ps_5_0".
		if (first)
		{
			first = false;
			if (line.find('_') != std::string::npos && line.find(' ') == std::string::npos)
			{
				out.profile = line;
				// Strip a trailing carriage return if the listing has CRLFs.
				while (!out.profile.empty() && std::isspace(static_cast<unsigned char>(out.profile.back())))
					out.profile.pop_back();
				continue;
			}
		}

		// The opcode runs up to the first space.
		std::string opcode = line.substr(0, line.find(' '));
		while (!opcode.empty() && std::isspace(static_cast<unsigned char>(opcode.back())))
			opcode.pop_back();
		if (opcode.empty())
			continue;

		// dcl_temps tells us the register pressure the compiler settled on.
		if (opcode == "dcl_temps")
		{
			const size_t space = line.find(' ');
			if (space != std::string::npos)
				out.temp_registers = static_cast<uint32_t>(std::strtoul(line.c_str() + space + 1, nullptr, 10));
		}

		const dxbc_category c = classify_dxbc_opcode(opcode);
		out.by_opcode[opcode]++;

		if (c == dxbc_category::decl)
		{
			out.declarations++;
			continue;
		}

		out.counts[static_cast<size_t>(c)]++;
		out.instructions++;
	}

	return true;
}

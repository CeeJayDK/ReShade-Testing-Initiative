/*
 * Rewrites applied to ReShade-generated HLSL before it is handed to
 * vkd3d-shader, for constructs D3DCompiler accepts and vkd3d does not.
 *
 * Only vkd3d's input changes: --hlsl output and the D3DCompiler path still see
 * ReShade's HLSL unchanged. Shared by the reshadefx_cli_fixed / coverage back
 * end (cli/effect_codegen_dxbc_vkd3d.cpp) and fxstat
 * (fxstat/src/core/dxbc_compile_vkd3d.cpp).
 */

#pragma once

#include <cctype>
#include <string>

inline void apply_vkd3d_hlsl_fixups(std::string &hlsl)
{
	// codegen_hlsl marks every [loop] loop [fastopt] at shader model 4+, and
	// vkd3d-shader aborts on that attribute (E5017, docs/upstream/vkd3d/
	// ISSUE-fastopt.md). To D3DCompiler it means [loop] plus a faster, less
	// thorough compile, so hand vkd3d [loop].
	for (size_t pos = 0; (pos = hlsl.find("[fastopt]", pos)) != std::string::npos; pos += 6)
		hlsl.replace(pos, 9, "[loop]");

	// vkd3d-shader has no isnan (E5005, docs/upstream/vkd3d/ISSUE-isnan.md).
	// Under IEEE rules NaN is the only value not equal to itself, so
	// isnan(x) is (x != x), per component for vectors too. codegen_hlsl
	// always writes isnan(<variable name>); anything else is left alone.
	const auto is_name_char = [](char c) { return c == '_' || std::isalnum(static_cast<unsigned char>(c)); };
	for (size_t pos = 0; (pos = hlsl.find("isnan(", pos)) != std::string::npos; ++pos)
	{
		if (pos != 0 && is_name_char(hlsl[pos - 1]))
			continue;
		const size_t arg = pos + 6;
		size_t end = arg;
		while (end < hlsl.size() && is_name_char(hlsl[end]))
			++end;
		if (end == arg || end >= hlsl.size() || hlsl[end] != ')')
			continue;
		const std::string name = hlsl.substr(arg, end - arg);
		hlsl.replace(pos, end + 1 - pos, '(' + name + " != " + name + ')');
	}
}

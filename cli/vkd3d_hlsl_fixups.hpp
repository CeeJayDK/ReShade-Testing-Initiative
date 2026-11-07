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
#include <initializer_list>
#include <string>

inline void apply_vkd3d_hlsl_fixups(std::string &hlsl)
{
	// codegen_hlsl marks every [loop] loop [fastopt] at shader model 4+, and
	// vkd3d-shader aborts on that attribute (E5017, docs/upstream/vkd3d/
	// ISSUE-fastopt.md). To D3DCompiler it means [loop] plus a faster, less
	// thorough compile, so hand vkd3d [loop].
	for (size_t pos = 0; (pos = hlsl.find("[fastopt]", pos)) != std::string::npos; pos += 6)
		hlsl.replace(pos, 9, "[loop]");

	// codegen_hlsl calls these intrinsics on one operand: a variable name, or a
	// cast of one such as ((uint)x), neither with side effects. Rewrite each
	// call with a single balanced argument; anything else is left alone.
	const auto is_name_char = [](char c) { return c == '_' || std::isalnum(static_cast<unsigned char>(c)); };
	const auto rewrite_calls = [&](const std::string &function, auto replacement) {
		const std::string call = function + '(';
		for (size_t pos = 0; (pos = hlsl.find(call, pos)) != std::string::npos; ++pos)
		{
			if (pos != 0 && is_name_char(hlsl[pos - 1]))
				continue;
			const size_t arg = pos + call.size();
			size_t end = arg;
			int depth = 0;
			for (; end < hlsl.size(); ++end)
			{
				const char c = hlsl[end];
				if (c == '(')
					++depth;
				else if (c == ')' && depth-- == 0)
					break;
				else if (c == ',' && depth == 0)
					break;
				else if (c == ';' || c == '{' || c == '}')
					break;
			}
			if (end == arg || end >= hlsl.size() || hlsl[end] != ')')
				continue;
			const std::string text = replacement(hlsl.substr(arg, end - arg));
			hlsl.replace(pos, end + 1 - pos, text);
			pos += text.size() - 1;
		}
	};

	// vkd3d-shader has no isnan (E5005, docs/upstream/vkd3d/ISSUE-isnan.md).
	// Under IEEE rules NaN is the only value not equal to itself, so
	// isnan(x) is (x != x), per component for vectors too.
	rewrite_calls("isnan", [](const std::string &name) { return '(' + name + " != " + name + ')'; });

	// vkd3d-shader cannot compile isinf outside the entry point at shader
	// model 4+, nor anywhere at shader model 3: E5017 "not yet implemented
	// feature: SM4 isinf expression" (docs/upstream/vkd3d/ISSUE-isinf.md).
	// Only +-infinity is larger in magnitude than the largest finite float,
	// and NaN compares false, so this is isinf for every value, per component.
	rewrite_calls("isinf", [](const std::string &name) { return "(abs(" + name + ") > 3.40282347e+38)"; });

	// vkd3d-shader has no reversebits (E5002 on its <error type> result,
	// docs/upstream/vkd3d/ISSUE-reversebits.md). Call the same parallel bit
	// swap ReShade uses below shader model 5, defined ahead of the rest.
	bool uses_reversebits = false;
	rewrite_calls("reversebits", [&](const std::string &name) { uses_reversebits = true; return "__rti_reversebits(" + name + ')'; });
	if (uses_reversebits)
	{
		std::string helpers;
		for (const char *n : { "", "2", "3", "4" })
			helpers += std::string("uint") + n + " __rti_reversebits(uint" + n + " v) {"
				" v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);"
				" v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);"
				" v = ((v >> 4) & 0x0F0F0F0Fu) | ((v & 0x0F0F0F0Fu) << 4);"
				" v = ((v >> 8) & 0x00FF00FFu) | ((v & 0x00FF00FFu) << 8);"
				" return (v >> 16) | (v << 16); }\n";
		hlsl.insert(0, helpers);
	}
}

/*
 * HLSL -> DXBC, through whichever compiler is available.
 *
 * Both ReShade DXBC back ends do the same two things: generate HLSL with
 * codegen_hlsl, then hand that text to a compiler. Doing the second step here
 * rather than in a codegen subclass means the compiler is a runtime choice
 * instead of a build-time one, and avoids linking two subclasses of
 * codegen_hlsl into the same binary.
 *
 *   D3DCompiler  Windows only, ships with the OS. What ReShade actually uses,
 *                so its instruction counts are the real ones for DX9-DX11.
 *   vkd3d-shader Any platform. Not a D3DCompiler clone -- it has no equivalent
 *                of D3DCOMPILE_OPTIMIZATION_LEVEL*, so its counts will not match
 *                D3DCompiler's and the two must never be compared.
 *
 * Which one produced a number is recorded in every result, because a report
 * that does not say cannot be compared with another one.
 */
#pragma once

#include "fxstat/fxstat.hpp"

#include <string>
#include <vector>

namespace fxstat
{
	// dxbc_compiler, available_dxbc_compilers() and dxbc_compiler_name() are
	// public API; see include/fxstat/fxstat.hpp.

	struct dxbc_compile_result
	{
		std::string binary;
		std::string assembly;
		std::string errors;
		std::string compiler_id;   // e.g. "vkd3d-shader" or "D3DCompiler_47"
		bool ok = false;
	};

	// `profile` is a full shader profile such as "ps_5_0".
	// `optimization_level` follows D3DCompiler: -1 disables optimisation, 0-3
	// select a level. vkd3d-shader ignores it and says so in the result.
	dxbc_compile_result compile_hlsl_to_dxbc(const std::string &hlsl,
	                                         const std::string &entry_point,
	                                         const std::string &profile,
	                                         dxbc_compiler which,
	                                         int optimization_level);
}

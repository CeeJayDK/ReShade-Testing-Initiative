/*
 * Compiler selection and dispatch. The two back ends live in their own files so
 * that each is compiled only where it can work.
 */

#include "dxbc_compile.hpp"

namespace fxstat
{
	// Defined in dxbc_compile_vkd3d.cpp / dxbc_compile_d3dcompiler.cpp.
#ifdef FXSTAT_HAVE_VKD3D
	dxbc_compile_result compile_hlsl_to_dxbc_vkd3d(const std::string &, const std::string &,
	                                               const std::string &);
#endif
#ifdef FXSTAT_HAVE_D3DCOMPILER
	dxbc_compile_result compile_hlsl_to_dxbc_d3dcompiler(const std::string &, const std::string &,
	                                                     const std::string &, int);
#endif
}

const char *fxstat::dxbc_compiler_name(dxbc_compiler c)
{
	switch (c)
	{
	case dxbc_compiler::vkd3d:       return "vkd3d-shader";
	case dxbc_compiler::d3dcompiler: return "D3DCompiler";
	default:                         return "automatic";
	}
}

std::vector<fxstat::dxbc_compiler> fxstat::available_dxbc_compilers()
{
	std::vector<dxbc_compiler> out;
	// D3DCompiler first: it is what ReShade actually uses, so where it exists it
	// is the more faithful answer.
#ifdef FXSTAT_HAVE_D3DCOMPILER
	out.push_back(dxbc_compiler::d3dcompiler);
#endif
#ifdef FXSTAT_HAVE_VKD3D
	out.push_back(dxbc_compiler::vkd3d);
#endif
	return out;
}

fxstat::dxbc_compile_result fxstat::compile_hlsl_to_dxbc(const std::string &hlsl,
	const std::string &entry_point, const std::string &profile,
	dxbc_compiler which, int optimization_level)
{
	if (which == dxbc_compiler::automatic)
	{
		const std::vector<dxbc_compiler> available = available_dxbc_compilers();
		if (available.empty())
		{
			dxbc_compile_result r;
			r.errors = "error: this build has no DXBC compiler. On Linux, build "
			           "libvkd3d-shader.a with build-vkd3d-shader.sh and re-run cmake "
			           "with -DVKD3D_BUILD=<dir>.\n";
			return r;
		}
		which = available.front();
	}

#ifdef FXSTAT_HAVE_D3DCOMPILER
	if (which == dxbc_compiler::d3dcompiler)
		return compile_hlsl_to_dxbc_d3dcompiler(hlsl, entry_point, profile, optimization_level);
#endif
#ifdef FXSTAT_HAVE_VKD3D
	if (which == dxbc_compiler::vkd3d)
		return compile_hlsl_to_dxbc_vkd3d(hlsl, entry_point, profile);
#endif

	dxbc_compile_result r;
	r.errors = std::string("error: the ") + dxbc_compiler_name(which) +
	           " back end is not available in this build.\n";
	return r;
}

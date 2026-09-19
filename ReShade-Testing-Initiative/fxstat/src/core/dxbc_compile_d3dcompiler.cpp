/*
 * HLSL -> DXBC via Microsoft's D3DCompiler. Windows only.
 *
 * This is the compiler ReShade itself uses, so on Windows these are the real
 * instruction counts for DX9 through DX12 -- not an approximation of them. It is
 * also the only route that supports shader model 3 pixel shaders, which
 * vkd3d-shader currently rejects.
 *
 * d3dcompiler_47.dll ships with Windows, so nothing extra is needed at run time.
 * The DLL is loaded lazily rather than linked, so a build that cannot find it
 * still starts and reports the problem instead of failing to load.
 *
 * The error de-duplication and the X3579 filter below match what
 * source/effect_codegen_dxbc.cpp does upstream; D3DCompiler repeats itself, and
 * warns about 'groupshared' in VS/PS modules that ReShade generates deliberately.
 */

#include "dxbc_compile.hpp"

#include <windows.h>
#include <d3dcompiler.h>

#include <string>
#include <string_view>

namespace
{
	using pfn_D3DCompile = HRESULT (WINAPI *)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *,
		ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);
	using pfn_D3DDisassemble = HRESULT (WINAPI *)(LPCVOID, SIZE_T, UINT, LPCSTR, ID3DBlob **);

	struct d3dcompiler_library
	{
		HMODULE module = nullptr;
		pfn_D3DCompile compile = nullptr;
		pfn_D3DDisassemble disassemble = nullptr;
		std::string name;

		d3dcompiler_library()
		{
			// Newest first. 47 is what ships with every supported Windows.
			for (const char *candidate : { "d3dcompiler_47.dll", "d3dcompiler_46.dll", "d3dcompiler_43.dll" })
			{
				module = ::LoadLibraryA(candidate);
				if (module != nullptr)
				{
					name = candidate;
					break;
				}
			}
			if (module == nullptr)
				return;

			compile = reinterpret_cast<pfn_D3DCompile>(
				reinterpret_cast<void *>(::GetProcAddress(module, "D3DCompile")));
			disassemble = reinterpret_cast<pfn_D3DDisassemble>(
				reinterpret_cast<void *>(::GetProcAddress(module, "D3DDisassemble")));
		}
	};

	const d3dcompiler_library &library()
	{
		static const d3dcompiler_library instance;
		return instance;
	}

	// D3DCompiler repeats identical error lines, and warns about 'groupshared'
	// in vertex/pixel modules (X3579) which ReShade emits on purpose.
	std::string tidy_errors(std::string text)
	{
		for (size_t line_offset = 0, next_line_offset;
			(next_line_offset = text.find('\n', line_offset)) != std::string::npos;
			line_offset = next_line_offset + 1)
		{
			const std::string_view cur_line(text.data() + line_offset, next_line_offset - line_offset);

			if (const size_t end_offset = text.find('\n', next_line_offset + 1);
				end_offset != std::string::npos)
			{
				const std::string_view next_line(text.data() + next_line_offset + 1,
					end_offset - next_line_offset - 1);
				if (cur_line == next_line)
				{
					text.erase(next_line_offset, end_offset - next_line_offset);
					next_line_offset = line_offset - 1;
					continue;
				}
			}

			if (cur_line.find("X3579") != std::string_view::npos)
			{
				text.erase(line_offset, next_line_offset + 1 - line_offset);
				next_line_offset = line_offset - 1;
			}
		}
		return text;
	}

	std::string blob_to_string(ID3DBlob *blob, bool drop_terminator)
	{
		if (blob == nullptr)
			return {};
		SIZE_T size = blob->GetBufferSize();
		if (drop_terminator && size > 0)
			size -= 1; // do not keep the NUL
		return std::string(static_cast<const char *>(blob->GetBufferPointer()), size);
	}
}

namespace fxstat
{
dxbc_compile_result compile_hlsl_to_dxbc_d3dcompiler(const std::string &hlsl,
	const std::string &entry_point, const std::string &profile, int optimization_level)
{
	dxbc_compile_result result;

	const d3dcompiler_library &lib = library();
	if (lib.compile == nullptr)
	{
		result.errors = "error: could not load d3dcompiler_47.dll (or D3DCompile is missing from it)\n";
		result.compiler_id = "D3DCompiler (unavailable)";
		return result;
	}
	result.compiler_id = lib.name;

	UINT flags = 0;
	if (optimization_level < 0)
		flags |= D3DCOMPILE_SKIP_OPTIMIZATION;
	else if (optimization_level >= 3)
		flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
	else if (optimization_level == 2)
		flags |= D3DCOMPILE_OPTIMIZATION_LEVEL2;
	else if (optimization_level == 1)
		flags |= D3DCOMPILE_OPTIMIZATION_LEVEL1;
	else
		flags |= D3DCOMPILE_OPTIMIZATION_LEVEL0;

	// Shader model 4 and up: match what the ReShade runtime asks for.
	if (profile.size() > 3 && profile[3] >= '4')
		flags |= D3DCOMPILE_ENABLE_STRICTNESS;

	ID3DBlob *compiled = nullptr;
	ID3DBlob *errors = nullptr;

	const HRESULT hr = lib.compile(hlsl.data(), hlsl.size(), nullptr, nullptr, nullptr,
		entry_point.c_str(), profile.c_str(), flags, 0, &compiled, &errors);

	if (errors != nullptr)
	{
		result.errors += tidy_errors(blob_to_string(errors, true));
		errors->Release();
	}

	if (FAILED(hr) || compiled == nullptr)
	{
		char buffer[64];
		std::snprintf(buffer, sizeof(buffer), "0x%08lx", static_cast<unsigned long>(hr));
		result.errors += "error: D3DCompile failed for entry point '" + entry_point +
			"' as " + profile + " (" + buffer + ")\n";
		if (compiled != nullptr)
			compiled->Release();
		return result;
	}

	result.binary = blob_to_string(compiled, false);

	if (lib.disassemble != nullptr)
	{
		ID3DBlob *disassembled = nullptr;
		if (SUCCEEDED(lib.disassemble(result.binary.data(), result.binary.size(), 0, nullptr, &disassembled)))
		{
			result.assembly = blob_to_string(disassembled, true);
			disassembled->Release();
		}
	}
	if (result.assembly.empty())
		result.errors += "warning: D3DDisassemble produced nothing for '" + entry_point +
			"', so it cannot be counted\n";

	compiled->Release();
	result.ok = true;
	return result;
}
} // namespace fxstat

/*
 * HLSL -> DXBC via vkd3d-shader. Works on any platform; the only DXBC route on
 * Linux, since D3DCompiler is a Windows DLL.
 *
 * Caveat that matters for every number this produces: vkd3d-shader has no
 * equivalent of D3DCOMPILE_OPTIMIZATION_LEVEL*. Its output is therefore not
 * instruction-for-instruction what D3DCompiler produces, and the two must never
 * be compared against each other. Compare vkd3d against vkd3d, and pin the
 * vkd3d revision.
 */

#include "dxbc_compile.hpp"

#include <vkd3d_shader.h>

#include <cstring>
#include <vector>

namespace
{
	struct scoped_shader_code
	{
		struct vkd3d_shader_code code = {};
		~scoped_shader_code() { vkd3d_shader_free_shader_code(&code); }
	};
	struct scoped_messages
	{
		char *messages = nullptr;
		~scoped_messages() { vkd3d_shader_free_messages(messages); }
	};

	const char *result_to_string(int result)
	{
		switch (result)
		{
		case VKD3D_OK:                     return "ok";
		case VKD3D_ERROR:                  return "unspecified error";
		case VKD3D_ERROR_OUT_OF_MEMORY:    return "out of memory";
		case VKD3D_ERROR_INVALID_ARGUMENT: return "invalid argument";
		case VKD3D_ERROR_INVALID_SHADER:   return "invalid shader";
		case VKD3D_ERROR_NOT_IMPLEMENTED:  return "not implemented by vkd3d-shader";
		default:                           return "unknown error";
		}
	}

	// Shader model 1-3 is the old DX9 bytecode container, not DXBC.
	bool is_sm1(const std::string &profile)
	{
		// "ps_3_0" -> major version digit at index 3.
		return profile.size() > 3 && profile[3] < '4';
	}
}

namespace fxstat
{
dxbc_compile_result compile_hlsl_to_dxbc_vkd3d(const std::string &hlsl,
	const std::string &entry_point, const std::string &profile)
{
	dxbc_compile_result result;
	result.compiler_id = "vkd3d-shader";

	const bool sm1 = is_sm1(profile);

	std::vector<struct vkd3d_shader_compile_option> options;
	if (sm1)
	{
		// ReShade's SM3 output uses the old semantic names (POSITION, COLOR,
		// TEXCOORD) and relies on D3DCompiler's legacy handling of them.
		options.push_back({ VKD3D_SHADER_COMPILE_OPTION_BACKWARD_COMPATIBILITY,
			VKD3D_SHADER_COMPILE_OPTION_BACKCOMPAT_MAP_SEMANTIC_NAMES });
	}

	struct vkd3d_shader_hlsl_source_info hlsl_info = {};
	hlsl_info.type = VKD3D_SHADER_STRUCTURE_TYPE_HLSL_SOURCE_INFO;
	hlsl_info.entry_point = entry_point.c_str();
	hlsl_info.profile = profile.c_str();

	struct vkd3d_shader_compile_info info = {};
	info.type = VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;
	info.next = &hlsl_info;
	info.source.code = hlsl.data();
	info.source.size = hlsl.size();
	info.source_type = VKD3D_SHADER_SOURCE_HLSL;
	info.target_type = sm1 ? VKD3D_SHADER_TARGET_D3D_BYTECODE : VKD3D_SHADER_TARGET_DXBC_TPF;
	info.options = options.empty() ? nullptr : options.data();
	info.option_count = static_cast<unsigned int>(options.size());
	info.log_level = VKD3D_SHADER_LOG_WARNING;
	info.source_name = entry_point.c_str();

	scoped_shader_code compiled;
	scoped_messages compile_messages;

	const int rc = vkd3d_shader_compile(&info, &compiled.code, &compile_messages.messages);

	if (compile_messages.messages != nullptr && *compile_messages.messages != '\0')
		result.errors += compile_messages.messages;

	if (rc != VKD3D_OK)
	{
		result.errors += "error: vkd3d-shader failed to compile entry point '" + entry_point +
			"' as " + profile + ": " + result_to_string(rc) + '\n';
		return result;
	}

	result.binary.assign(static_cast<const char *>(compiled.code.code), compiled.code.size);

	// Disassembly is best effort: a module that compiled but will not
	// disassemble is still valid, so do not fail the build for it. It is needed
	// for instruction counting though, so its absence is not silent.
	struct vkd3d_shader_compile_info dis = {};
	dis.type = VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;
	dis.source = compiled.code;
	dis.source_type = sm1 ? VKD3D_SHADER_SOURCE_D3D_BYTECODE : VKD3D_SHADER_SOURCE_DXBC_TPF;
	dis.target_type = VKD3D_SHADER_TARGET_D3D_ASM;
	dis.log_level = VKD3D_SHADER_LOG_NONE;
	dis.source_name = entry_point.c_str();

	scoped_shader_code disassembled;
	scoped_messages dis_messages;

	if (vkd3d_shader_compile(&dis, &disassembled.code, &dis_messages.messages) == VKD3D_OK)
		result.assembly.assign(static_cast<const char *>(disassembled.code.code), disassembled.code.size);
	else
		result.errors += "warning: vkd3d-shader could not disassemble '" + entry_point +
			"', so it cannot be counted\n";

	result.ok = true;
	return result;
}
} // namespace fxstat

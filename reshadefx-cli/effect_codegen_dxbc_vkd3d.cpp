/*
 * A DXBC back end for ReShadeFX that does not need Microsoft's D3DCompiler.
 *
 * crosire's source/effect_codegen_dxbc.cpp calls D3DCompile(), so the --dxbc
 * target of ReShadeFXC only exists on Windows. This is a drop-in replacement
 * built on vkd3d-shader, which compiles HLSL to DXBC natively on any platform:
 * it provides the same reshadefx::create_codegen_dxbc() symbol and subclasses
 * the same codegen_hlsl, so it can simply be compiled in place of the original.
 *
 * Shader model mapping:
 *   30       -> vs_3_0 / ps_3_0, emitted as D3D bytecode (the DX9 format)
 *   40..51   -> vs_5_0 / ps_5_0 / cs_5_0 etc., emitted as DXBC ("TPF")
 *
 * Differences from D3DCompiler worth knowing about:
 *
 *   - vkd3d-shader has no equivalent of D3DCOMPILE_OPTIMIZATION_LEVEL*. The
 *     optimization_level argument is accepted for signature compatibility and
 *     is only used to honour '#pragma reshade skipoptimization', which maps to
 *     the closest available behaviour. Instruction counts from this back end
 *     therefore will not match FXC's exactly.
 *   - Its HLSL front end is not a complete D3DCompiler clone. Effects that
 *     compile with FXC can still fail here; that is a vkd3d limitation, not a
 *     ReShadeFX one, and the error text says so.
 */

#define RESHADEFX_CODEGEN_HLSL_INLINE

#include "effect_codegen_hlsl.cpp"

#include <vkd3d_shader.h>

#include <cstring>
#include <string>
#include <vector>

using namespace reshadefx;

namespace
{
	// vkd3d_shader_free_* on scope exit, so the error paths below cannot leak.
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

	const char *vkd3d_result_to_string(int result)
	{
		switch (result)
		{
		case VKD3D_OK:                            return "ok";
		case VKD3D_ERROR:                         return "unspecified error";
		case VKD3D_ERROR_OUT_OF_MEMORY:           return "out of memory";
		case VKD3D_ERROR_INVALID_ARGUMENT:        return "invalid argument";
		case VKD3D_ERROR_INVALID_SHADER:          return "invalid shader";
		case VKD3D_ERROR_NOT_IMPLEMENTED:         return "not implemented by vkd3d-shader";
		default:                                  return "unknown error";
		}
	}
}

class codegen_dxbc final : public codegen_hlsl
{
public:
	codegen_dxbc(unsigned int shader_model, bool debug_info, bool uniforms_to_spec_constants, int optimization_level) :
		codegen_hlsl(shader_model, debug_info, uniforms_to_spec_constants),
		_optimization_level(optimization_level)
	{
	}

	bool assemble_code_for_entry_point(const std::string &entry_point_name, std::string &cso, std::string &assembly, std::string &errors) const override
	{
		const auto entry_point_it = std::find_if(_module.entry_points.begin(), _module.entry_points.end(),
			[&entry_point_name](const std::pair<std::string, shader_type> &entry_point) {
				return entry_point.first == entry_point_name;
			});
		if (entry_point_it == _module.entry_points.end())
			return false;

		// Generate the HLSL for this entry point first, exactly as the Windows
		// back end does; only the step after it differs.
		std::string hlsl;
		if (!codegen_hlsl::assemble_code_for_entry_point(entry_point_name, hlsl, hlsl, errors))
			return false;

		char profile[] = "cs_0_0";
		profile[0] = entry_point_it->second == shader_type::vertex ? 'v' : entry_point_it->second == shader_type::pixel ? 'p' : 'c';
		profile[3] = static_cast<char>('0' + (_shader_model / 10) % 10);
		profile[5] = static_cast<char>('0' + (_shader_model % 10));

		// Shader model 1-3 is the old DX9 bytecode container, not DXBC.
		const bool is_sm1 = _shader_model < 40;
		const enum vkd3d_shader_target_type target_type =
			is_sm1 ? VKD3D_SHADER_TARGET_D3D_BYTECODE : VKD3D_SHADER_TARGET_DXBC_TPF;

		std::vector<struct vkd3d_shader_compile_option> options;
		if (is_sm1)
		{
			// ReShade's SM3 output uses the old semantic names (POSITION,
			// COLOR, TEXCOORD) and relies on D3DCompiler's legacy handling of
			// them. vkd3d needs to be told to do the same.
			options.push_back({ VKD3D_SHADER_COMPILE_OPTION_BACKWARD_COMPATIBILITY,
				VKD3D_SHADER_COMPILE_OPTION_BACKCOMPAT_MAP_SEMANTIC_NAMES });
		}

		struct vkd3d_shader_hlsl_source_info hlsl_info = {};
		hlsl_info.type = VKD3D_SHADER_STRUCTURE_TYPE_HLSL_SOURCE_INFO;
		hlsl_info.entry_point = entry_point_name.c_str();
		hlsl_info.profile = profile;

		struct vkd3d_shader_compile_info info = {};
		info.type = VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;
		info.next = &hlsl_info;
		info.source.code = hlsl.data();
		info.source.size = hlsl.size();
		info.source_type = VKD3D_SHADER_SOURCE_HLSL;
		info.target_type = target_type;
		info.options = options.empty() ? nullptr : options.data();
		info.option_count = static_cast<unsigned int>(options.size());
		info.log_level = VKD3D_SHADER_LOG_WARNING;
		info.source_name = entry_point_name.c_str();

		scoped_shader_code compiled;
		scoped_messages compile_messages;

		const int result = vkd3d_shader_compile(&info, &compiled.code, &compile_messages.messages);

		if (compile_messages.messages != nullptr && *compile_messages.messages != '\0')
			errors += compile_messages.messages;

		if (result != VKD3D_OK)
		{
			errors += "error: vkd3d-shader failed to compile entry point '" + entry_point_name +
				"' as " + profile + ": " + vkd3d_result_to_string(result) + '\n';
			return false;
		}

		cso.assign(static_cast<const char *>(compiled.code.code), compiled.code.size);

		// Disassembly is best effort: a module that compiled but will not
		// disassemble is still a valid module, so do not fail the build for it.
		struct vkd3d_shader_compile_info dis_info = {};
		dis_info.type = VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;
		dis_info.source = compiled.code;
		dis_info.source_type = is_sm1 ? VKD3D_SHADER_SOURCE_D3D_BYTECODE : VKD3D_SHADER_SOURCE_DXBC_TPF;
		dis_info.target_type = VKD3D_SHADER_TARGET_D3D_ASM;
		dis_info.log_level = VKD3D_SHADER_LOG_NONE;
		dis_info.source_name = entry_point_name.c_str();

		scoped_shader_code disassembled;
		scoped_messages dis_messages;

		if (vkd3d_shader_compile(&dis_info, &disassembled.code, &dis_messages.messages) == VKD3D_OK)
			assembly.assign(static_cast<const char *>(disassembled.code.code), disassembled.code.size);

		return true;
	}

	void emit_pragma(const std::string &pragma) override
	{
		if (pragma == "reshade skipoptimization" || pragma == "reshade nooptimization")
			_optimization_level = -1;

		codegen_hlsl::emit_pragma(pragma);
	}

private:
	int _optimization_level;
};

#ifndef RESHADEFX_CODEGEN_DXBC_INLINE
codegen *reshadefx::create_codegen_dxbc(unsigned int shader_model, bool debug_info, bool uniforms_to_spec_constants, int optimization_level)
{
	return new codegen_dxbc(shader_model, debug_info, uniforms_to_spec_constants, optimization_level);
}
#endif

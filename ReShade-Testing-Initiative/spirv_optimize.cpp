#include "spirv_optimize.hpp"

#ifdef RESHADEFX_HAVE_SPIRV_TOOLS

#include <spirv-tools/optimizer.hpp>
#include <spirv-tools/libspirv.h>

namespace
{
	// Deliberately NOT spirv-opt -O.
	//
	// -O is wrong here for two reasons. It does not run --freeze-spec-const, so
	// the specialization constants keep their symbolic form and not one dead
	// branch is removed -- on LumaSharpen, -O leaves all 15 texture fetches
	// standing. And it inlines and unrolls, so counts move for reasons unrelated
	// to the source change being measured, which makes it useless for diffing.
	//
	// These passes do only what every driver does unconditionally before
	// reaching its own back end: bake the constants, inline (GPUs have no call
	// stack), promote locals to SSA, fold, remove the branches the constants made
	// dead, and sweep up.
	const char *const k_passes[] = {
		"--freeze-spec-const",
		"--fold-spec-const-op-composite",
		"--eliminate-dead-functions",
		"--inline-entry-points-exhaustive",
		"--private-to-local",
		"--scalar-replacement=100",
		"--eliminate-local-multi-store",
		"--ccp",
		"--eliminate-dead-branches",
		"--simplify-instructions",
		"--vector-dce",
		"--eliminate-local-multi-store",
		"--eliminate-dead-branches",
		"--merge-blocks",
		"--cfg-cleanup",
		"--redundancy-elimination",
		"--eliminate-dead-code-aggressive",
	};

	constexpr uint32_t k_spirv_magic = 0x07230203;
	constexpr uint16_t k_op_function_parameter = 55;

	// A surviving function parameter means the inliner declined the module.
	bool has_function_parameter(const std::vector<uint32_t> &words)
	{
		if (words.size() < 5 || words[0] != k_spirv_magic)
			return false;
		for (size_t i = 5; i < words.size();)
		{
			const uint16_t opcode = static_cast<uint16_t>(words[i] & 0xFFFF);
			const uint16_t length = static_cast<uint16_t>(words[i] >> 16);
			if (length == 0 || i + length > words.size())
				break;
			if (opcode == k_op_function_parameter)
				return true;
			i += length;
		}
		return false;
	}
}

bool reshadefx_tools::spirv_optimize_available()
{
	return true;
}

std::string reshadefx_tools::spirv_optimize_version()
{
	return spvSoftwareVersionString();
}

std::vector<std::string> reshadefx_tools::spirv_optimize_passes()
{
	return std::vector<std::string>(std::begin(k_passes), std::end(k_passes));
}

bool reshadefx_tools::spirv_optimize(const std::string &input, std::string &output,
                                     bool &inliner_incomplete, std::string &error)
{
	inliner_incomplete = false;

	if (input.size() % 4 != 0)
	{
		error = "SPIR-V binary size is not a multiple of 4";
		return false;
	}

	// reshadefx_rga asks reshadefx for OpenGL-semantics SPIR-V
	// (create_codegen_spirv with vulkan_semantics=false), which legitimately
	// uses gl_VertexID and OriginLowerLeft. Validating that against Vulkan rules
	// rejects every module. Use the universal environment, which imposes neither
	// API's extra rules.
	spvtools::Optimizer optimizer(SPV_ENV_UNIVERSAL_1_3);

	std::string messages;
	optimizer.SetMessageConsumer(
		[&messages](spv_message_level_t level, const char *, const spv_position_t &, const char *message) {
			if (level > SPV_MSG_WARNING)
				return;
			messages += message;
			messages += '\n';
		});

	if (!optimizer.RegisterPassesFromFlags(spirv_optimize_passes()))
	{
		error = "SPIRV-Tools rejected the pass list";
		if (!messages.empty())
			error += ": " + messages;
		return false;
	}

	// Skip the input validator outright. We are measuring a module ReShade
	// already produced and ships, not vetting it -- and reshadefx emits
	// API-specific constructs that a mismatched validator environment rejects.
	std::vector<uint32_t> result;
	spvtools::ValidatorOptions validator_options;
	if (!optimizer.Run(reinterpret_cast<const uint32_t *>(input.data()), input.size() / 4, &result,
	                   validator_options, /* skip_validation = */ true))
	{
		error = "SPIR-V optimization failed";
		if (!messages.empty())
			error += ": " + messages;
		return false;
	}
	if (result.empty())
	{
		error = "optimizer produced an empty module";
		return false;
	}

	inliner_incomplete = has_function_parameter(result);
	output.assign(reinterpret_cast<const char *>(result.data()), result.size() * 4);
	return true;
}

#else // RESHADEFX_HAVE_SPIRV_TOOLS

bool reshadefx_tools::spirv_optimize_available()
{
	return false;
}

std::string reshadefx_tools::spirv_optimize_version()
{
	return {};
}

std::vector<std::string> reshadefx_tools::spirv_optimize_passes()
{
	return {};
}

bool reshadefx_tools::spirv_optimize(const std::string &, std::string &, bool &, std::string &error)
{
	error = "this build has no SPIRV-Tools; rebuild with SPIRV-Tools available "
	        "(Debian/Ubuntu: apt-get install spirv-tools) to use --optimize";
	return false;
}

#endif

/*
 * The driver-equivalent SPIR-V optimisation pass, run through the SPIRV-Tools
 * C++ API rather than by spawning spirv-opt.
 *
 * Linking it in rather than shelling out buys three things: it works in a WASM
 * build, where there is no process to spawn; the pass list and the SPIRV-Tools
 * version are pinned into the binary, so two machines cannot silently produce
 * different counts; and it removes a per-entry-point process spawn.
 */

#include "optimize.hpp"
#include "fxstat/fxstat.hpp"

#include <spirv-tools/optimizer.hpp>
#include <spirv-tools/libspirv.h>

namespace
{
	// Deliberately NOT -O.
	//
	// -O is wrong here for two reasons. It does not run --freeze-spec-const, so
	// the specialisation constants keep their symbolic form and not one dead
	// branch is removed -- on LumaSharpen, -O leaves all 15 texture fetches
	// standing. And it inlines and unrolls, so counts move for reasons
	// unrelated to the source change being measured, which makes it useless for
	// diffing.
	//
	// These passes do only what every driver does unconditionally before it
	// reaches its own back end: bake the constants, inline (GPUs have no call
	// stack), promote locals to SSA, fold, remove the branches the constants
	// made dead, and sweep up.
	const char *const k_driver_passes[] = {
		// Performance mode: uniforms are constants, so make them literal constants.
		"--freeze-spec-const",
		"--fold-spec-const-op-composite",
		"--eliminate-dead-functions",
		// GPUs have no call stack -- every driver inlines everything.
		"--inline-entry-points-exhaustive",
		// mem2reg. ReShadeFX emits everything through OpVariable/OpLoad/OpStore;
		// nothing folds until locals are promoted to SSA values.
		"--private-to-local",
		"--scalar-replacement=100",
		"--eliminate-local-multi-store",
		// Propagate the now-constant values and kill what they made unreachable.
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
}

std::vector<std::string> fxstat::driver_pass_list()
{
	return std::vector<std::string>(std::begin(k_driver_passes), std::end(k_driver_passes));
}

namespace fxstat
{
	std::string optimizer_version()
	{
		return spvSoftwareVersionString();
	}
}

bool fxstat::optimize_spirv(const std::string &input, std::string &output, std::string &error)
{
	if (input.size() % 4 != 0)
	{
		error = "SPIR-V binary size is not a multiple of 4";
		return false;
	}

	spvtools::Optimizer optimizer(SPV_ENV_VULKAN_1_1);

	std::string messages;
	optimizer.SetMessageConsumer(
		[&messages](spv_message_level_t level, const char *, const spv_position_t &position, const char *message) {
			if (level > SPV_MSG_WARNING)
				return; // info and debug chatter is not useful here
			messages += std::string(level == SPV_MSG_WARNING ? "warning: " : "error: ");
			if (position.index != 0)
				messages += "at " + std::to_string(position.index) + ": ";
			messages += message;
			messages += '\n';
		});

	const std::vector<std::string> passes = driver_pass_list();
	if (!optimizer.RegisterPassesFromFlags(passes))
	{
		error = "SPIRV-Tools rejected the pass list";
		if (!messages.empty())
			error += ": " + messages;
		return false;
	}

	std::vector<uint32_t> result;
	const bool ok = optimizer.Run(
		reinterpret_cast<const uint32_t *>(input.data()), input.size() / 4, &result);

	if (!ok)
	{
		error = "SPIR-V optimisation failed";
		if (!messages.empty())
			error += ": " + messages;
		return false;
	}

	output.assign(reinterpret_cast<const char *>(result.data()), result.size() * 4);
	if (output.empty())
	{
		error = "optimiser produced an empty module";
		return false;
	}
	return true;
}

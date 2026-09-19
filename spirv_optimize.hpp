/*
 * Optional SPIR-V optimization for reshadefx_rga.
 *
 * Why this exists: reshadefx's SPIR-V backend never optimizes. It emits both
 * sides of every `if` unconditionally, so `--perf` alone barely moves the
 * numbers (LumaSharpen: 117 -> 116 ALU). ReShade ships that raw SPIR-V straight
 * to the Vulkan driver and lets the driver's own compiler do the work. Counting
 * the unoptimized module therefore counts code no GPU ever executes.
 *
 * `--optimize` runs the folding, inlining, mem2reg and dead-branch passes that
 * every driver runs before its own back end, so the count is of code that
 * actually executes. On LumaSharpen that is 116 -> 15 ALU and 16 -> 5 texture.
 *
 * This is a VENDOR-NEUTRAL PROXY, not a reproduction of any particular driver.
 * No two drivers optimize identically. Use it to compare one version of a
 * shader against another, not to predict absolute cost on a given GPU.
 *
 * Requires SPIRV-Tools. It is linked, not invoked as a subprocess, so the pass
 * list and the SPIRV-Tools version are pinned into the binary and two machines
 * cannot silently produce different numbers. Build without
 * RESHADEFX_HAVE_SPIRV_TOOLS and --optimize reports that it is unavailable
 * instead of disappearing.
 */
#pragma once

#include <string>
#include <vector>

namespace reshadefx_tools
{
	// True when this build has SPIRV-Tools linked in.
	bool spirv_optimize_available();

	// The pass list, so a report can record exactly what produced its numbers.
	std::vector<std::string> spirv_optimize_passes();

	// Version string of the linked SPIRV-Tools, or "" when unavailable.
	std::string spirv_optimize_version();

	// Run the pass list over `input`. Returns false and fills `error` on failure.
	//
	// `inliner_incomplete` is set when an OpFunctionParameter survives: the
	// SPIRV-Tools inliner declines some modules, and when it does mem2reg has
	// nothing to work on, the load/store scaffolding survives and the counts come
	// out badly inflated (on SMAA, by a factor of five). A caller that does not
	// check this will report a number five times too large with no indication
	// anything went wrong.
	bool spirv_optimize(const std::string &input, std::string &output,
	                    bool &inliner_incomplete, std::string &error);
}

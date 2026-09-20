/*
 * Real GPU ISA statistics via AMD's Radeon GPU Analyzer.
 *
 * Native only, and deliberately in the CLI rather than the library: it spawns a
 * process and writes temporary files, which the library never does (so it can
 * still be built for WASM). RGA is not bundled -- it is a large AMD binary under
 * AMD's own EULA. Download it from
 * https://github.com/GPUOpen-Tools/radeon_gpu_analyzer/releases and pass --rga.
 *
 * RGA's vk-spv-offline mode runs the same LLPC compiler AMD's Vulkan driver
 * uses, offline, with no GPU and no driver installed, on Windows and Linux.
 * That makes this the only number in fxstat that is what a GPU actually runs:
 * scalar instructions after the driver's own optimiser, register allocation and
 * spilling. Everything else is a pre-driver estimate.
 */
#pragma once

#include "fxstat/fxstat.hpp"

#include <cstdint>
#include <map>
#include <string>

namespace fxstat
{
	struct isa_stats
	{
		bool ok = false;
		std::string error;

		uint32_t valu = 0;      // vector ALU, one per lane-wide operation (includes trans)
		uint32_t trans = 0;     // v_exp/log/rcp/rsq/sqrt/sin/cos: quarter rate
		uint32_t salu = 0;      // scalar ALU: once per wave, not per pixel
		uint32_t vmem = 0;      // image/buffer/global loads and stores (texture fetches)
		uint32_t smem = 0;      // scalar memory loads (constant buffers)
		uint32_t scratch = 0;   // scratch_* instructions: spilled/indexed memory, VRAM traffic
		uint32_t branch = 0;    // s_branch / s_cbranch_*
		uint32_t total = 0;     // every instruction in the listing

		uint32_t vgprs = 0;     // decide occupancy: fewer = more waves in flight
		uint32_t sgprs = 0;
		uint32_t scratch_bytes = 0;
		uint32_t vgpr_spills = 0;
		uint32_t isa_bytes = 0;

		// Rough relative cost of one invocation: every VALU instruction once,
		// transcendentals weighted 4x in total (they issue at quarter rate).
		// Ignores memory latency, which is usually what dominates -- compare
		// vmem and scratch separately.
		uint32_t cost() const { return valu + 3 * trans; }
	};

	struct rga_options
	{
		std::string executable;           // path to rga / rga.exe
		std::string asic = "gfx1100";     // RDNA3; `rga -s vk-spv-offline -l` lists all
	};

	// Compile every entry point's SPIR-V binary with RGA. `result` must come from
	// the SPIR-V back end with keep_binaries set. Keyed by entry point name.
	std::map<std::string, isa_stats> run_rga(const rga_options &options, const compile_result &result);

	// Parse an RGA ISA listing and its resource-usage CSV. Exposed for tests.
	isa_stats parse_rga_output(const std::string &isa_text, const std::string &csv_text);
}

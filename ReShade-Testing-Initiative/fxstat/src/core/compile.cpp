/*
 * The compile pipeline: preprocess, parse, bake the preset in, assemble each
 * entry point, optimise, count.
 *
 * No printf, no exit(), no process spawning, no writing to disk. Everything the
 * caller needs comes back in the result struct.
 */

#include "fxstat/fxstat.hpp"

#include "optimize.hpp"
#include "spirv_patch.hpp"
#include "spirv_stats.hpp"
#include "dxbc_stats.hpp"
#include "dxbc_compile.hpp"

#include "effect_parser.hpp"
#include "effect_codegen.hpp"
#include "effect_preprocessor.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <sstream>

namespace
{

std::string trim(const std::string &s)
{
	const size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos)
		return {};
	const size_t e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

std::vector<std::string> split_values(const std::string &value)
{
	std::vector<std::string> parts;
	std::stringstream ss(value);
	std::string item;
	while (std::getline(ss, item, ','))
		parts.push_back(trim(item));
	return parts;
}

// Overwrite a spec constant's initializer with the preset value, the way
// runtime.cpp does before calling finalize_code().
//
// `scalar_component` selects which component of a vector-valued preset entry
// this spec constant carries. The SPIR-V back end flattens every uniform into
// one scalar spec constant per component (each with its own SpecId) and records
// the component index in `offset`; the HLSL-derived back ends keep the uniform
// whole. Pass -1 for the whole-uniform case.
void apply_preset_value(reshadefx::uniform &sc, const std::string &raw, int scalar_component)
{
	const std::vector<std::string> parts = split_values(raw);

	const auto assign = [&sc](unsigned int slot, const std::string &v) {
		switch (sc.type.base)
		{
		case reshadefx::type::t_bool:
			sc.initializer_value.as_uint[slot] =
				(v == "1" || v == "true" || v == "True" || v == "TRUE") ? 1 : 0;
			break;
		case reshadefx::type::t_int:
		case reshadefx::type::t_min16int:
			sc.initializer_value.as_int[slot] = std::atoi(v.c_str());
			break;
		case reshadefx::type::t_uint:
		case reshadefx::type::t_min16uint:
			sc.initializer_value.as_uint[slot] =
				static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
			break;
		case reshadefx::type::t_float:
		case reshadefx::type::t_min16float:
			sc.initializer_value.as_float[slot] = static_cast<float>(std::atof(v.c_str()));
			break;
		default:
			break;
		}
	};

	if (scalar_component >= 0)
	{
		if (static_cast<size_t>(scalar_component) < parts.size())
			assign(0, parts[scalar_component]);
		return;
	}

	const unsigned int components = std::min<unsigned int>(
		static_cast<unsigned int>(parts.size()), sc.type.components());
	for (unsigned int i = 0; i < components; ++i)
		assign(i, parts[i]);
}

std::string format_uniform_value(const reshadefx::uniform &u)
{
	std::string result;
	for (unsigned int i = 0; i < u.type.components(); ++i)
	{
		if (i != 0)
			result += ',';
		char buffer[64];
		switch (u.type.base)
		{
		case reshadefx::type::t_bool:
			std::snprintf(buffer, sizeof(buffer), "%s", u.initializer_value.as_uint[i] ? "true" : "false");
			break;
		case reshadefx::type::t_int:
		case reshadefx::type::t_min16int:
			std::snprintf(buffer, sizeof(buffer), "%d", u.initializer_value.as_int[i]);
			break;
		case reshadefx::type::t_uint:
		case reshadefx::type::t_min16uint:
			std::snprintf(buffer, sizeof(buffer), "%u", u.initializer_value.as_uint[i]);
			break;
		default:
			std::snprintf(buffer, sizeof(buffer), "%g", u.initializer_value.as_float[i]);
			break;
		}
		result += buffer;
	}
	return result;
}

const char *stage_name(reshadefx::shader_type type)
{
	switch (type)
	{
	case reshadefx::shader_type::vertex:  return "vertex";
	case reshadefx::shader_type::pixel:   return "pixel";
	case reshadefx::shader_type::compute: return "compute";
	default:                              return "unknown";
	}
}

// The preprocessor environment the ReShade runtime sets up. Getting this wrong
// is not a cosmetic problem: an undefined __RENDERER__ evaluates to 0 in #if, so
// every renderer-conditional effect silently compiles its fallback path.
void setup_preprocessor(reshadefx::preprocessor &pp, const fxstat::compile_options &o,
                        unsigned int renderer)
{
	// User defines go FIRST. add_macro_definition() keeps the first definition of
	// a name, and runtime.cpp relies on that same rule to let preset definitions
	// override the built-in ones.
	for (const auto &[name, value] : o.defines)
		pp.add_macro_definition(name, value);

	pp.add_macro_definition("__RESHADE__", "60800");
	pp.add_macro_definition("__RESHADE_PERMUTATION__", "0");
	pp.add_macro_definition("__RESHADE_PERFORMANCE_MODE__", o.performance_mode ? "1" : "0");
	pp.add_macro_definition("__VENDOR__", "0");
	pp.add_macro_definition("__DEVICE__", "0");
	pp.add_macro_definition("__RENDERER__", std::to_string(renderer));
	pp.add_macro_definition("__APPLICATION__", "0");
	pp.add_macro_definition("BUFFER_WIDTH", std::to_string(o.width));
	pp.add_macro_definition("BUFFER_HEIGHT", std::to_string(o.height));
	pp.add_macro_definition("BUFFER_RCP_WIDTH", "(1.0 / BUFFER_WIDTH)");
	pp.add_macro_definition("BUFFER_RCP_HEIGHT", "(1.0 / BUFFER_HEIGHT)");
	pp.add_macro_definition("BUFFER_COLOR_SPACE", "1");
	pp.add_macro_definition("BUFFER_COLOR_FORMAT", "28");
	pp.add_macro_definition("BUFFER_COLOR_BIT_DEPTH", "8");

	for (const std::string &path : o.include_paths)
		pp.add_include_path(std::filesystem::u8path(path));

	// The same backwards-compatibility macros runtime.cpp injects. Without them
	// any effect still using the pre-5.0 spelling of the offset and gather
	// intrinsics fails to compile, which is most older shader packs.
	pp.append_string(
		"#define tex2Doffset(s, coords, offset) tex2D(s, coords, offset)\n"
		"#define tex2Dlodoffset(s, coords, offset) tex2Dlod(s, coords, offset)\n"
		"#define tex2Dgather(s, t, c) tex2Dgather##c(s, t)\n"
		"#define tex2Dgatheroffset(s, t, o, c) tex2Dgather##c(s, t, o)\n"
		"#define tex2Dgather0 tex2DgatherR\n"
		"#define tex2Dgather1 tex2DgatherG\n"
		"#define tex2Dgather2 tex2DgatherB\n"
		"#define tex2Dgather3 tex2DgatherA\n");
}

reshadefx::codegen *make_codegen(const fxstat::compile_options &o)
{
	switch (o.target)
	{
	case fxstat::backend::spirv:
		return reshadefx::create_codegen_spirv(true, false, o.performance_mode, false, false);
	case fxstat::backend::hlsl:
		return reshadefx::create_codegen_hlsl(o.shader_model, false, o.performance_mode);
	case fxstat::backend::glsl:
		return reshadefx::create_codegen_glsl(true, false, o.performance_mode, false, false);
	case fxstat::backend::dxbc:
		// The DXBC back ends are codegen_hlsl plus a call to a compiler. Use the
		// plain HLSL codegen and do the compile step ourselves, so the compiler
		// is a runtime choice and no two subclasses of codegen_hlsl end up in
		// the same binary.
		return reshadefx::create_codegen_hlsl(o.shader_model, false, o.performance_mode);
	}
	return nullptr;
}

// Shared tail: everything after the source has been preprocessed.
fxstat::compile_result compile_preprocessed(reshadefx::preprocessor &pp,
                                            const fxstat::compile_options &o,
                                            unsigned int renderer)
{
	fxstat::compile_result result;
	result.renderer_used = renderer;
	result.optimizer_version = fxstat::optimizer_version();
	result.warnings = pp.errors();

	std::unique_ptr<reshadefx::codegen> codegen(make_codegen(o));
	if (!codegen)
	{
		result.errors = "error: this build has no DXBC back end. Build libvkd3d-shader.a "
		                "with build-vkd3d-shader.sh and re-run cmake with -DVKD3D_BUILD=<dir>.\n";
		return result;
	}

	reshadefx::parser parser;
	if (!parser.parse(pp.output(), codegen.get()))
	{
		result.errors = parser.errors();
		return result;
	}
	if (!parser.errors().empty())
		result.warnings += parser.errors();

	// Bake the preset into the spec constants, as runtime.cpp does before
	// finalize_code(). Statistics in performance mode are a property of the
	// preset, not of the effect.
	std::vector<uint32_t> spec_constant_values; // indexed by SpecId
	if (o.performance_mode)
	{
		const bool flattened = (o.target == fxstat::backend::spirv);

		for (reshadefx::uniform &sc : codegen->module().spec_constants)
		{
			if (const auto it = o.preset.find(sc.name); it != o.preset.end())
				apply_preset_value(sc, it->second, flattened ? static_cast<int>(sc.offset) : -1);

			spec_constant_values.push_back(sc.initializer_value.as_uint[0]);

			std::string label = sc.name;
			if (flattened && sc.type.components() == 1 && !result.uniform_values.empty() &&
				result.uniform_values.back().first.compare(0, label.size(), label) == 0)
				label += '[' + std::to_string(sc.offset) + ']';
			result.uniform_values.emplace_back(label, format_uniform_value(sc));
		}
	}

	const std::string generated = codegen->finalize_code();
	if (o.keep_generated_code)
		result.generated_code = generated;

	for (const std::pair<std::string, reshadefx::shader_type> &ep : codegen->module().entry_points)
	{
		std::string binary, assembly, errors;
		if (!codegen->assemble_code_for_entry_point(ep.first, binary, assembly, errors))
		{
			result.errors += "error: could not assemble entry point '" + ep.first + "': " + errors + '\n';
			return result;
		}
		if (!errors.empty())
			result.warnings += errors;

		fxstat::entry_point_result e;
		e.name = ep.first;
		e.stage = stage_name(ep.second);

		if (o.target == fxstat::backend::dxbc)
		{
			// codegen_hlsl put the HLSL for this entry point in `binary`; compile it.
			char profile[] = "cs_0_0";
			profile[0] = ep.second == reshadefx::shader_type::vertex ? 'v'
			           : ep.second == reshadefx::shader_type::pixel  ? 'p' : 'c';
			profile[3] = static_cast<char>('0' + (o.shader_model / 10) % 10);
			profile[5] = static_cast<char>('0' + (o.shader_model % 10));

			const fxstat::dxbc_compile_result dxbc_out = fxstat::compile_hlsl_to_dxbc(
				binary, ep.first, profile, o.dxbc, o.optimization_level);

			if (result.dxbc_compiler_used.empty())
				result.dxbc_compiler_used = dxbc_out.compiler_id;

			if (!dxbc_out.ok)
			{
				result.errors += dxbc_out.errors;
				return result;
			}
			if (!dxbc_out.errors.empty())
				result.warnings += dxbc_out.errors;

			binary = dxbc_out.binary;
			assembly = dxbc_out.assembly;

			// The compiler already optimised this, so there is no "before
			// optimisation" column and nothing further to run.
			fxstat::dxbc_stats dxbc;
			std::string error;
			if (!fxstat::analyze_dxbc_asm(assembly, dxbc, error))
			{
				result.errors += "error: " + error + " (entry point '" + ep.first + "')\n";
				return result;
			}

			e.stats.counts[static_cast<size_t>(fxstat::category::tex)]  = dxbc.get(fxstat::dxbc_category::tex);
			e.stats.counts[static_cast<size_t>(fxstat::category::alu)]  = dxbc.get(fxstat::dxbc_category::alu);
			e.stats.counts[static_cast<size_t>(fxstat::category::mov)]  = dxbc.get(fxstat::dxbc_category::mov);
			e.stats.counts[static_cast<size_t>(fxstat::category::mem)]  = dxbc.get(fxstat::dxbc_category::mem);
			e.stats.counts[static_cast<size_t>(fxstat::category::flow)] = dxbc.get(fxstat::dxbc_category::flow);
			e.stats.total = dxbc.instructions;
			e.stats.by_opcode = dxbc.by_opcode;
			e.profile = dxbc.profile;
			e.temp_registers = dxbc.temp_registers;
		}
		else if (o.target == fxstat::backend::spirv)
		{
			// ReShade's SPIR-V back end leaves performance-mode values to
			// VkSpecializationInfo, so without this the module still carries the
			// defaults written in the effect source, whatever the preset says.
			if (!spec_constant_values.empty())
			{
				std::string patch_error;
				if (!fxstat::patch_spec_constants(binary, spec_constant_values, patch_error))
				{
					result.errors += "error: " + patch_error + '\n';
					return result;
				}
			}

			std::string error;
			if (!fxstat::analyze_spirv(binary, e.stats_unoptimized, nullptr, error))
			{
				result.errors += "error: " + error + " (entry point '" + ep.first + "')\n";
				return result;
			}

			if (o.optimize)
			{
				std::string optimized;
				if (!fxstat::optimize_spirv(binary, optimized, error))
				{
					result.errors += "error: " + error + " (entry point '" + ep.first + "')\n";
					return result;
				}
				if (!fxstat::analyze_spirv(optimized, e.stats, &e.inliner_incomplete, error))
				{
					result.errors += "error: " + error + '\n';
					return result;
				}
				e.has_unoptimized = true;
				if (o.keep_binaries)
					binary = optimized;
			}
			else
			{
				e.stats = e.stats_unoptimized;
			}
		}
		else
		{
			// HLSL and GLSL produce text, not something to count. The generated
			// code is still available for feeding to a native compiler.
			result.warnings += "note: instruction counting is implemented for the SPIR-V and "
			                   "DXBC back ends; use the generated code for " +
			                   std::string(fxstat::backend_name(o.target)) + ".\n";
			break;
		}

		if (o.keep_binaries)
		{
			e.binary = std::move(binary);
			e.assembly = std::move(assembly);
		}

		result.entry_points.push_back(std::move(e));
	}

	result.ok = true;
	return result;
}

} // namespace

// ---------------------------------------------------------------------------

unsigned int fxstat::default_renderer(backend target, unsigned int shader_model)
{
	switch (target)
	{
	case backend::spirv: return renderer_vulkan;
	case backend::glsl:  return renderer_opengl;
	default: break;
	}
	if (shader_model >= 51) return renderer_d3d12;
	if (shader_model >= 50) return renderer_d3d11;
	if (shader_model >= 41) return renderer_d3d10_1;
	if (shader_model >= 40) return renderer_d3d10;
	return renderer_d3d9;
}

const char *fxstat::backend_name(backend b)
{
	switch (b)
	{
	case backend::spirv: return "SPIR-V";
	case backend::hlsl:  return "HLSL";
	case backend::glsl:  return "GLSL";
	default:             return "DXBC";
	}
}

fxstat::compile_result fxstat::compile_file(const std::string &path, const compile_options &options)
{
	const unsigned int renderer = options.renderer != 0
		? options.renderer : default_renderer(options.target, options.shader_model);

	reshadefx::preprocessor pp;
	setup_preprocessor(pp, options, renderer);
	// The effect's own directory is always on the include path.
	pp.add_include_path(std::filesystem::u8path(path).parent_path());

	if (!pp.append_file(std::filesystem::u8path(path)))
	{
		compile_result result;
		result.renderer_used = renderer;
		result.errors = pp.errors();
		if (result.errors.empty())
			result.errors = "error: could not read '" + path + "'\n";
		return result;
	}

	return compile_preprocessed(pp, options, renderer);
}

fxstat::compile_result fxstat::compile_source(const std::string &source, const std::string &name,
                                              const compile_options &options)
{
	const unsigned int renderer = options.renderer != 0
		? options.renderer : default_renderer(options.target, options.shader_model);

	reshadefx::preprocessor pp;
	setup_preprocessor(pp, options, renderer);

	if (!pp.append_string(source, std::filesystem::u8path(name)))
	{
		compile_result result;
		result.renderer_used = renderer;
		result.errors = pp.errors();
		return result;
	}

	return compile_preprocessed(pp, options, renderer);
}

std::map<std::string, std::string> fxstat::parse_preset(const std::string &ini_text,
                                                        const std::string &section)
{
	std::map<std::string, std::string> out;
	std::stringstream stream(ini_text);
	std::string line, current;

	while (std::getline(stream, line))
	{
		line = trim(line);
		if (line.empty() || line[0] == ';' || line[0] == '#')
			continue;
		if (line.front() == '[' && line.back() == ']')
		{
			current = line.substr(1, line.size() - 2);
			continue;
		}
		const size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		if (!section.empty() && current != section)
			continue;
		out[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
	}
	return out;
}

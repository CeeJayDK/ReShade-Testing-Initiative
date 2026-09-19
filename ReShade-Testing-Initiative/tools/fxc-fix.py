#!/usr/bin/env python3
"""
Apply the ReShadeFXC environment fix to crosire/reshade's tools/fxc.cpp.

tools/fxc.cpp sets up a preprocessor environment that diverges from the one
source/runtime.cpp sets up before compiling the same effects. The result is that
ReShadeFXC rejects effects the runtime compiles fine, and silently compiles the
wrong branch of others.

usage: fxc-fix.py <path to reshade checkout>
"""
import re
import sys
import pathlib

OLD = '''	pp.add_macro_definition("__RESHADE__", std::to_string(VERSION_MAJOR * 10000 + VERSION_MINOR * 100 + VERSION_REVISION));
	pp.add_macro_definition("__RESHADE_PERFORMANCE_MODE__", "0");
	pp.add_macro_definition("BUFFER_WIDTH", buffer_width);
	pp.add_macro_definition("BUFFER_HEIGHT", buffer_height);
	pp.add_macro_definition("BUFFER_RCP_WIDTH", "(1.0 / BUFFER_WIDTH)");
	pp.add_macro_definition("BUFFER_RCP_HEIGHT", "(1.0 / BUFFER_HEIGHT)");

	if (!pp.append_file(source_file))
'''

NEW = '''	// Mirror the renderer ID the runtime would report for the selected back end,
	// so that effects branching on __RENDERER__ take the same path here as they
	// do in ReShade. Left undefined, __RENDERER__ evaluates to 0 in #if and every
	// such effect silently compiles its fallback path.
	unsigned int renderer_id;
	if (generate_spirv)
		renderer_id = 0x20000; // Vulkan
	else if (generate_glsl)
		renderer_id = vulkan_semantics ? 0x20000 : 0x10000; // Vulkan / OpenGL
	else if (shader_model >= 51)
		renderer_id = 0xc000; // D3D12
	else if (shader_model >= 50)
		renderer_id = 0xb000; // D3D11
	else if (shader_model >= 41)
		renderer_id = 0xa100; // D3D10.1
	else if (shader_model >= 40)
		renderer_id = 0xa000; // D3D10
	else
		renderer_id = 0x9000; // D3D9

	pp.add_macro_definition("__RESHADE__", std::to_string(VERSION_MAJOR * 10000 + VERSION_MINOR * 100 + VERSION_REVISION));
	pp.add_macro_definition("__RESHADE_PERMUTATION__", "0");
	// Specialization constants are what performance mode is, so report it as on
	// when it was asked for. Otherwise an effect can be compiled with its
	// uniforms folded away while __RESHADE_PERFORMANCE_MODE__ still says 0.
	pp.add_macro_definition("__RESHADE_PERFORMANCE_MODE__", spec_constants ? "1" : "0");
	pp.add_macro_definition("__VENDOR__", "0");
	pp.add_macro_definition("__DEVICE__", "0");
	pp.add_macro_definition("__RENDERER__", std::to_string(renderer_id));
	pp.add_macro_definition("__APPLICATION__", "0");
	pp.add_macro_definition("BUFFER_WIDTH", buffer_width);
	pp.add_macro_definition("BUFFER_HEIGHT", buffer_height);
	pp.add_macro_definition("BUFFER_RCP_WIDTH", "(1.0 / BUFFER_WIDTH)");
	pp.add_macro_definition("BUFFER_RCP_HEIGHT", "(1.0 / BUFFER_HEIGHT)");
	pp.add_macro_definition("BUFFER_COLOR_SPACE", "1");   // unknown/sRGB
	pp.add_macro_definition("BUFFER_COLOR_FORMAT", "28"); // r8g8b8a8_unorm
	pp.add_macro_definition("BUFFER_COLOR_BIT_DEPTH", "8");

	// Add some conversion macros for compatibility with older versions of ReShade.
	// Identical to the block in runtime.cpp; without it any effect still using the
	// pre-5.0 spelling of the offset and gather intrinsics fails to compile here
	// while compiling fine in ReShade itself.
	pp.append_string(
		"#define tex2Doffset(s, coords, offset) tex2D(s, coords, offset)\\n"
		"#define tex2Dlodoffset(s, coords, offset) tex2Dlod(s, coords, offset)\\n"
		"#define tex2Dgather(s, t, c) tex2Dgather##c(s, t)\\n"
		"#define tex2Dgatheroffset(s, t, o, c) tex2Dgather##c(s, t, o)\\n"
		"#define tex2Dgather0 tex2DgatherR\\n"
		"#define tex2Dgather1 tex2DgatherG\\n"
		"#define tex2Dgather2 tex2DgatherB\\n"
		"#define tex2Dgather3 tex2DgatherA\\n");

	if (!pp.append_file(source_file))
'''


# --- fix 2: --invert-y is passed in the wrong argument position -------------
#
# The signature is
#   create_codegen_glsl/spirv(vulkan_semantics, debug_info,
#                             uniforms_to_spec_constants,
#                             enable_16bit_types = false,
#                             flip_vert_y = false)
# so passing invert_y_axis as the 4th argument sets enable_16bit_types.
# --invert-y therefore does not invert Y, and does silently switch min16float
# and friends to real 16-bit types, changing the precision of the output.

ARG_FIXES = [
    ("reshadefx::create_codegen_glsl(vulkan_semantics, debug_info, spec_constants, invert_y_axis)",
     "reshadefx::create_codegen_glsl(vulkan_semantics, debug_info, spec_constants, false, invert_y_axis)"),
    ("reshadefx::create_codegen_spirv(vulkan_semantics, debug_info, spec_constants, invert_y_axis)",
     "reshadefx::create_codegen_spirv(vulkan_semantics, debug_info, spec_constants, false, invert_y_axis)"),
]

# --- fixes 3-5: the output stage --------------------------------------------
#
# 3. `--spirv` without -E writes a zero-byte file and exits 0. The SPIR-V back
#    end's finalize_code() returns an empty string by design (there is no text
#    form of a SPIR-V module), and fxc.cpp writes it regardless.
#
# 4. `-E <name>` with a name that does not exist fails with exit 1 and NO
#    message at all -- assemble_code_for_entry_point() just returns false and
#    the three error strings are all empty. Since ReShade mangles entry point
#    names, and mangles them differently per back end (the HLSL back end keeps
#    the 'F' prefix at shader model 4+, while SPIR-V and SM3 rename to 'E'),
#    there is no way for a user to guess the right name. List them.
#
# 5. The disassembly that assemble_code_for_entry_point() produces is computed
#    and then dropped on the floor. FXC spells that option -Fc; do the same.

OLD_FINALIZE = '''	std::basic_string<char> code;
	if (entry_point_name != nullptr)
	{
		std::basic_string<char> assembly, errors;
		if (!backend->assemble_code_for_entry_point(entry_point_name, code, assembly, errors))
		{
			if (error_file == nullptr)
				std::cout << pp.errors() << parser.errors() << errors << std::endl;
			else
				std::ofstream(error_file) << pp.errors() << parser.errors() << errors;
			return 1;
		}
	}
	else
	{
		code = backend->finalize_code();
	}
'''

NEW_FINALIZE = '''	// Entry point names are generated, and the spelling depends on the back end
	// and shader model, so a user cannot guess them. Always be ready to list them.
	const auto list_entry_points = [&backend]() -> std::string {
		std::string list = "Available entry points:\\n";
		for (const std::pair<std::string, reshadefx::shader_type> &ep : backend->module().entry_points)
		{
			const char *kind =
				ep.second == reshadefx::shader_type::vertex ? "vertex" :
				ep.second == reshadefx::shader_type::pixel ? "pixel" :
				ep.second == reshadefx::shader_type::compute ? "compute" : "unknown";
			list += "  " + ep.first + " (" + kind + ")\\n";
		}
		return list;
	};

	const auto report = [&](const std::string &message) {
		if (error_file == nullptr)
			std::cout << pp.errors() << parser.errors() << message << std::endl;
		else
			std::ofstream(error_file) << pp.errors() << parser.errors() << message;
	};

	if (list_entry_points_only)
	{
		std::cout << list_entry_points();
		return 0;
	}

	std::basic_string<char> code, assembly;
	if (entry_point_name != nullptr)
	{
		const bool known = std::any_of(
			backend->module().entry_points.begin(), backend->module().entry_points.end(),
			[entry_point_name](const std::pair<std::string, reshadefx::shader_type> &ep) {
				return ep.first == entry_point_name;
			});

		std::basic_string<char> errors;
		if (!backend->assemble_code_for_entry_point(entry_point_name, code, assembly, errors))
		{
			// Without this, an unknown -E name exits 1 printing nothing at all.
			if (!known)
				errors += std::string("error: no entry point named '") + entry_point_name + "'\\n" + list_entry_points();
			report(errors);
			return 1;
		}
	}
	else
	{
		code = backend->finalize_code();

		// The SPIR-V back end has no text representation, so finalize_code()
		// returns nothing and a module can only be produced per entry point.
		// Say so rather than writing an empty file and reporting success.
		if (generate_spirv)
		{
			report("error: the SPIR-V back end produces one module per entry point; "
				"pass -E <name> to select one.\\n" + list_entry_points());
			return 1;
		}
	}

	// The back ends compute a disassembly listing and it was previously discarded.
	if (assembly_file != nullptr)
	{
		if (assembly.empty())
			std::cout << "warning: this back end produced no disassembly listing" << std::endl;
		else
			std::ofstream(assembly_file, std::ios::binary).write(assembly.data(), assembly.size());
	}
'''

# Declarations and argument parsing for -Fc / --list-entry-points.
DECL_OLD = '	const char *output_file = nullptr;\n'
DECL_NEW = ('	const char *output_file = nullptr;\n'
            '	const char *assembly_file = nullptr;\n'
            '	bool list_entry_points_only = false;\n')

ARG_OLD = '''			else if (0 == std::strcmp(arg, "-Fo"))
				output_file = argv[++i];
'''
ARG_NEW = '''			else if (0 == std::strcmp(arg, "-Fo"))
				output_file = argv[++i];
			else if (0 == std::strcmp(arg, "-Fc"))
				assembly_file = argv[++i];
'''

# --list-entry-points takes no value, so it belongs with the other flags that
# are matched after the "needs a value" group.
FLAG_OLD = '			if (0 == std::strcmp(arg, "-Zi"))\n'
FLAG_NEW = ('			if (0 == std::strcmp(arg, "--list-entry-points"))\n'
            '				list_entry_points_only = true;\n'
            '			else if (0 == std::strcmp(arg, "-Zi"))\n')

USAGE_OLD = '  -Fo <path>                Output generated code to a specific file.\n'
USAGE_NEW = ('  -Fo <path>                Output generated code to a specific file.\n'
             '  -Fc <path>                Output the disassembly listing to a specific file.\n'
             '  --list-entry-points       List the entry points in the effect and exit.\n')

# The new code uses std::any_of.
ALGO_OLD = '#include <iostream>\n'
ALGO_NEW = '#include <iostream>\n#include <algorithm>\n'

# --list-entry-points only inspects the module, so it needs neither an entry
# point nor an output file. Without this it trips the "--dxbc requires -E"
# check and prints the usage text instead.
VALIDATE_OLD = ('	if (source_file == nullptr || (generate_glsl && (generate_dxbc || generate_hlsl)) '
                '|| (generate_dxbc && entry_point_name == nullptr) '
                '|| (output_file == nullptr && (!generate_hlsl && !generate_glsl)))\n')
VALIDATE_NEW = ('	if (source_file == nullptr || (generate_glsl && (generate_dxbc || generate_hlsl)) '
                '|| (generate_dxbc && entry_point_name == nullptr && !list_entry_points_only) '
                '|| (output_file == nullptr && (!generate_hlsl && !generate_glsl) && !list_entry_points_only))\n')


def main():
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    path = root / "tools" / "fxc.cpp"
    text = path.read_text()

    if "tex2Doffset" in text:
        print(f"{path}: already patched, nothing to do")
        return 0
    if OLD not in text:
        print(f"{path}: the expected macro block was not found -- "
              f"fxc.cpp has changed and this patch needs updating", file=sys.stderr)
        return 1

    text = text.replace(OLD, NEW, 1)
    print(f"{path}: fixed preprocessor environment")

    for old, new in ARG_FIXES:
        if old not in text:
            print(f"  warning: could not apply argument-order fix:\n    {old}", file=sys.stderr)
            continue
        text = text.replace(old, new, 1)
    print(f"{path}: fixed --invert-y argument position")

    edits = [
        (DECL_OLD,     DECL_NEW,     "added -Fc / --list-entry-points state"),
        (ARG_OLD,      ARG_NEW,      "added -Fc argument"),
        (FLAG_OLD,     FLAG_NEW,     "added --list-entry-points argument"),
        (USAGE_OLD,    USAGE_NEW,    "documented the new options"),
        (ALGO_OLD,     ALGO_NEW,     "included <algorithm>"),
        (VALIDATE_OLD, VALIDATE_NEW, "let --list-entry-points bypass the -E/-Fo requirements"),
        (OLD_FINALIZE, NEW_FINALIZE, "fixed silent/empty output and added the disassembly listing"),
    ]
    for old, new, what in edits:
        if old not in text:
            print(f"  warning: could not apply: {what}", file=sys.stderr)
            return 1
        text = text.replace(old, new, 1)
        print(f"{path}: {what}")

    path.write_text(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())

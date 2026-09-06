// reshadefx_stats: compiles a .fx file with the real reshadefx frontend and
// reports real SPIR-V instruction counts per shader entry point.
// Built from crosire/reshade's unmodified source/effect_*.cpp files.
#include "effect_parser.hpp"
#include "effect_codegen.hpp"
#include "effect_preprocessor.hpp"
#include <cstring>
#include <cstdint>
#include <iostream>
#include <vector>

static size_t count_spirv_instructions(const std::string &binary)
{
	if (binary.size() < 20 || (binary.size() % 4) != 0)
		return 0;

	const auto *words = reinterpret_cast<const uint32_t *>(binary.data());
	const size_t word_count = binary.size() / 4;

	size_t count = 0;
	size_t i = 5; // skip the 5-word SPIR-V header
	while (i < word_count)
	{
		const uint32_t word_length = words[i] >> 16;
		if (word_length == 0)
			break;
		count++;
		i += word_length;
	}
	return count;
}

int main(int argc, char *argv[])
{
	if (argc < 2)
	{
		std::cerr << "usage: " << argv[0] << " [-D name=value] [-I path] <file.fx>\n";
		return 1;
	}

	reshadefx::preprocessor pp;
	pp.add_macro_definition("__RESHADE__", "60000");
	pp.add_macro_definition("__RESHADE_PERFORMANCE_MODE__", "0");
	pp.add_macro_definition("BUFFER_WIDTH", "1920");
	pp.add_macro_definition("BUFFER_HEIGHT", "1080");
	pp.add_macro_definition("BUFFER_RCP_WIDTH", "(1.0 / BUFFER_WIDTH)");
	pp.add_macro_definition("BUFFER_RCP_HEIGHT", "(1.0 / BUFFER_HEIGHT)");

	const char *source_file = nullptr;
	for (int i = 1; i < argc; ++i)
	{
		if (0 == std::strcmp(argv[i], "-D") && i + 1 < argc)
		{
			char *name = argv[++i];
			char *value = std::strchr(name, '=');
			if (value) *value++ = '\0';
			pp.add_macro_definition(name, value ? value : "1");
		}
		else if (0 == std::strcmp(argv[i], "-I") && i + 1 < argc)
		{
			pp.add_include_path(argv[++i]);
		}
		else
		{
			source_file = argv[i];
		}
	}

	if (source_file == nullptr)
	{
		std::cerr << "error: no input file\n";
		return 1;
	}

	if (!pp.append_file(source_file))
	{
		std::cout << pp.errors() << std::endl;
		return 1;
	}

	std::unique_ptr<reshadefx::codegen> backend(
		reshadefx::create_codegen_spirv(false, false, false, false));

	reshadefx::parser parser;
	if (!parser.parse(pp.output(), backend.get()))
	{
		std::cout << pp.errors() << parser.errors() << std::endl;
		return 1;
	}

	std::cout << "COMPILE_OK\n";

	size_t total = 0;
	for (const auto &[entry_name, stage] : backend->module().entry_points)
	{
		std::string binary, assembly, errors;
		const char *stage_name =
			stage == reshadefx::shader_type::vertex ? "vertex" :
			stage == reshadefx::shader_type::pixel ? "pixel" :
			stage == reshadefx::shader_type::compute ? "compute" : "unknown";

		if (!backend->assemble_code_for_entry_point(entry_name, binary, assembly, errors))
		{
			std::cout << "ENTRY_FAIL " << entry_name << " " << errors << "\n";
			continue;
		}

		const size_t count = count_spirv_instructions(binary);
		total += count;
		std::cout << "ENTRY " << stage_name << " " << entry_name
			<< " instructions=" << count << "\n";
	}
	std::cout << "TOTAL_INSTRUCTIONS " << total << "\n";

	return 0;
}

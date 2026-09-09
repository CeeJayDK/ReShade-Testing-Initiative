// reshadefx_stats: compiles a .fx file with the real reshadefx frontend and
// reports real SPIR-V instruction counts per shader entry point.
// Built from crosire/reshade's unmodified source/effect_*.cpp files.
#include "effect_parser.hpp"
#include "effect_codegen.hpp"
#include "effect_preprocessor.hpp"
#include "json_util.hpp"
#include <cstring>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <regex>
#include <vector>

// Parses reshadefx's diagnostic text lines into structured fields, e.g.:
//   file.fx(4, 11): error X3004: undeclared identifier 'foo'
//   file.fx(1, 1): preprocessor error: could not open included file 'x.fxh'
// A line that doesn't match this shape (rare, but not impossible) is still
// included with only its raw text set, rather than silently dropped.
struct diagnostic
{
	std::string file, line, column, severity, code, message, raw;
	bool matched = false;
};

static std::vector<diagnostic> parse_diagnostics(const std::string &text)
{
	static const std::regex pattern(
		R"(^(.+)\((\d+), (\d+)\): (preprocessor error|preprocessor warning|error|warning)(?: (\S+))?: (.*)$)");

	std::vector<diagnostic> out;
	std::istringstream stream(text);
	std::string line;
	while (std::getline(stream, line))
	{
		if (line.empty())
			continue;
		std::smatch m;
		diagnostic d;
		d.raw = line;
		if (std::regex_match(line, m, pattern))
		{
			d.matched = true;
			d.file = m[1];
			d.line = m[2];
			d.column = m[3];
			d.severity = m[4];
			d.code = m[5];
			d.message = m[6];
		}
		out.push_back(std::move(d));
	}
	return out;
}

static std::string diagnostics_to_json(const std::string &text)
{
	const std::vector<diagnostic> diags = parse_diagnostics(text);
	std::ostringstream out;
	out << "[";
	for (size_t i = 0; i < diags.size(); ++i)
	{
		const diagnostic &d = diags[i];
		if (i > 0) out << ",";
		out << "{\"raw\":\"" << json_escape(d.raw) << "\"";
		if (d.matched)
		{
			out << ",\"file\":\"" << json_escape(d.file) << "\""
				<< ",\"line\":" << d.line
				<< ",\"column\":" << d.column
				<< ",\"severity\":\"" << json_escape(d.severity) << "\"";
			if (!d.code.empty())
				out << ",\"code\":\"" << json_escape(d.code) << "\"";
			out << ",\"message\":\"" << json_escape(d.message) << "\"";
		}
		out << "}";
	}
	out << "]";
	return out.str();
}

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

// Baked in by the build script from RESHADE_VERSION (MAJOR*10000 + MINOR*100
// + REVISION, matching how ReShade itself computes __RESHADE__). Falls back
// to a placeholder if compiled directly without that define.
#ifndef RESHADEFX_VERSION_NUM
#define RESHADEFX_VERSION_NUM 60000
#endif

int main(int argc, char *argv[])
{
	if (argc < 2)
	{
		std::cerr << "usage: " << argv[0] << " [-D name=value] [-I path] [--json]\n"
			"       [--reshade-version <num>] [--performance-mode] [--width <n>] [--height <n>] <file.fx>\n";
		return 1;
	}

	reshadefx::preprocessor pp;

	// Defaults for the four ReShade-specific macros a shader might branch on.
	// Applied AFTER argument parsing below (mirroring how crosire's own
	// fxc.cpp does this) - add_macro_definition() silently keeps whichever
	// value was added FIRST for a given name, so a user override must reach
	// the preprocessor before these defaults do, not after.
	std::string reshade_version = std::to_string(RESHADEFX_VERSION_NUM);
	bool performance_mode = false;
	std::string buffer_width = "1920";
	std::string buffer_height = "1080";

	const char *source_file = nullptr;
	bool json_output = false;
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
		else if (0 == std::strcmp(argv[i], "--json"))
		{
			json_output = true;
		}
		else if (0 == std::strcmp(argv[i], "--reshade-version") && i + 1 < argc)
		{
			reshade_version = argv[++i];
		}
		else if (0 == std::strcmp(argv[i], "--performance-mode"))
		{
			performance_mode = true;
		}
		else if (0 == std::strcmp(argv[i], "--width") && i + 1 < argc)
		{
			buffer_width = argv[++i];
		}
		else if (0 == std::strcmp(argv[i], "--height") && i + 1 < argc)
		{
			buffer_height = argv[++i];
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

	// Apply defaults now, after parsing - see comment above main() for why
	// this order matters (a redefinition with a different value is silently
	// rejected, so any user override must be added to `pp` first).
	pp.add_macro_definition("__RESHADE__", reshade_version);
	pp.add_macro_definition("__RESHADE_PERFORMANCE_MODE__", performance_mode ? "1" : "0");
	pp.add_macro_definition("BUFFER_WIDTH", buffer_width);
	pp.add_macro_definition("BUFFER_HEIGHT", buffer_height);
	pp.add_macro_definition("BUFFER_RCP_WIDTH", "(1.0 / BUFFER_WIDTH)");
	pp.add_macro_definition("BUFFER_RCP_HEIGHT", "(1.0 / BUFFER_HEIGHT)");

	if (!pp.append_file(source_file))
	{
		if (json_output)
			std::cout << "{\"file\":\"" << json_escape(source_file) << "\",\"success\":false,"
				<< "\"stage\":\"preprocess\",\"diagnostics\":" << diagnostics_to_json(pp.errors()) << "}\n";
		else
			std::cout << pp.errors() << std::endl;
		return 1;
	}

	std::unique_ptr<reshadefx::codegen> backend(
		reshadefx::create_codegen_spirv(false, false, false, false));

	reshadefx::parser parser;
	if (!parser.parse(pp.output(), backend.get()))
	{
		if (json_output)
			std::cout << "{\"file\":\"" << json_escape(source_file) << "\",\"success\":false,"
				<< "\"stage\":\"parse\",\"diagnostics\":" << diagnostics_to_json(pp.errors() + parser.errors()) << "}\n";
		else
			std::cout << pp.errors() << parser.errors() << std::endl;
		return 1;
	}

	if (!json_output)
		std::cout << "COMPILE_OK\n";

	struct entry_result { std::string stage, name; size_t instructions; bool failed; std::string error; };
	std::vector<entry_result> results;

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
			results.push_back({stage_name, entry_name, 0, true, errors});
			if (!json_output)
				std::cout << "ENTRY_FAIL " << entry_name << " " << errors << "\n";
			continue;
		}

		const size_t count = count_spirv_instructions(binary);
		total += count;
		results.push_back({stage_name, entry_name, count, false, ""});
		if (!json_output)
			std::cout << "ENTRY " << stage_name << " " << entry_name
				<< " instructions=" << count << "\n";
	}

	if (json_output)
	{
		std::cout << "{\"file\":\"" << json_escape(source_file) << "\",\"success\":true,\"entries\":[";
		for (size_t i = 0; i < results.size(); ++i)
		{
			const auto &r = results[i];
			if (i > 0) std::cout << ",";
			std::cout << "{\"stage\":\"" << r.stage << "\",\"name\":\"" << json_escape(r.name) << "\"";
			if (r.failed)
				std::cout << ",\"failed\":true,\"error\":\"" << json_escape(r.error) << "\"}";
			else
				std::cout << ",\"instructions\":" << r.instructions << "}";
		}
		std::cout << "],\"total_instructions\":" << total << "}\n";
	}
	else
	{
		std::cout << "TOTAL_INSTRUCTIONS " << total << "\n";
	}

	return 0;
}

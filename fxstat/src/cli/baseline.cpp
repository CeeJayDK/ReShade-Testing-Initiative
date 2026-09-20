/*
 * Reading back a baseline written by --json.
 *
 * Deliberately a small targeted scanner rather than a JSON library: the only
 * documents it ever parses are ones fxstat itself wrote, and a dependency-free
 * build matters more here than generality. Anything it cannot make sense of is
 * reported as an error rather than silently treated as "no change".
 */

#include "baseline.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace
{
	// Find `"key"` after `from`, then the first number after the following colon.
	bool find_number(const std::string &s, const std::string &key, size_t from, size_t limit, uint32_t &out)
	{
		const std::string needle = '"' + key + '"';
		const size_t k = s.find(needle, from);
		if (k == std::string::npos || k >= limit)
			return false;
		const size_t colon = s.find(':', k + needle.size());
		if (colon == std::string::npos || colon >= limit)
			return false;
		size_t p = colon + 1;
		while (p < limit && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r'))
			++p;
		if (p >= limit || !(std::isdigit(static_cast<unsigned char>(s[p])) || s[p] == '-'))
			return false;
		out = static_cast<uint32_t>(std::strtoul(s.c_str() + p, nullptr, 10));
		return true;
	}

	bool find_string(const std::string &s, const std::string &key, size_t from, size_t limit, std::string &out)
	{
		const std::string needle = '"' + key + '"';
		const size_t k = s.find(needle, from);
		if (k == std::string::npos || k >= limit)
			return false;
		const size_t open = s.find('"', s.find(':', k + needle.size()) + 1);
		if (open == std::string::npos || open >= limit)
			return false;
		const size_t close = s.find('"', open + 1);
		if (close == std::string::npos || close > limit)
			return false;
		out = s.substr(open + 1, close - open - 1);
		return true;
	}
}

bool fxstat::load_baseline(const std::string &path, std::map<std::string, baseline_entry> &out, std::string &error)
{
	std::ifstream file(path);
	if (!file)
	{
		error = "could not open baseline '" + path + '\'';
		return false;
	}

	std::stringstream ss;
	ss << file.rdbuf();
	const std::string text = ss.str();

	const size_t list = text.find("\"entry_points\"");
	if (list == std::string::npos)
	{
		error = "'" + path + "' is not an fxstat --json report (no entry_points)";
		return false;
	}

	// Each entry point is an object starting at a '{' after the list.
	size_t pos = text.find('[', list);
	if (pos == std::string::npos)
	{
		error = "'" + path + "' has a malformed entry_points list";
		return false;
	}

	while (true)
	{
		const size_t open = text.find('{', pos);
		if (open == std::string::npos)
			break;
		// The object ends at the matching brace; nested "unoptimized" counts as one level.
		size_t close = open, depth = 0;
		for (; close < text.size(); ++close)
		{
			if (text[close] == '{') depth++;
			else if (text[close] == '}' && --depth == 0) break;
		}
		if (close >= text.size())
			break;

		baseline_entry entry;
		std::string name;
		if (find_string(text, "name", open, close, name))
		{
			find_string(text, "stage", open, close, entry.stage);
			// Read the top-level counts only: stop the search for each key at the
			// start of the first nested object ("unoptimized" or "isa").
			size_t counts_limit = close;
			for (const char *nested : { "\"unoptimized\"", "\"isa\"" })
			{
				const size_t n = text.find(nested, open);
				if (n != std::string::npos && n < counts_limit)
					counts_limit = n;
			}

			find_number(text, "tex", open, counts_limit, entry.tex);
			find_number(text, "alu", open, counts_limit, entry.alu);
			find_number(text, "mov", open, counts_limit, entry.mov);
			find_number(text, "mem", open, counts_limit, entry.mem);
			find_number(text, "flow", open, counts_limit, entry.flow);
			find_number(text, "total", open, counts_limit, entry.total);
			entry.has_lanes = find_number(text, "alu_lanes", open, counts_limit, entry.alu_lanes);
			find_number(text, "trans_lanes", open, counts_limit, entry.trans_lanes);

			const size_t isa = text.find("\"isa\"", open);
			if (isa != std::string::npos && isa < close)
			{
				const size_t isa_close = text.find('}', isa);
				entry.has_isa = find_number(text, "valu", isa, isa_close, entry.isa_valu);
				find_number(text, "trans", isa, isa_close, entry.isa_trans);
				find_number(text, "salu", isa, isa_close, entry.isa_salu);
				find_number(text, "vmem", isa, isa_close, entry.isa_vmem);
				find_number(text, "scratch", isa, isa_close, entry.isa_scratch);
				find_number(text, "vgprs", isa, isa_close, entry.isa_vgprs);
			}
			out[name] = entry;
		}

		pos = close + 1;
		const size_t next = text.find_first_not_of(" \t\r\n", pos);
		if (next == std::string::npos || text[next] != ',')
			break;
	}

	if (out.empty())
	{
		error = "'" + path + "' contained no entry points";
		return false;
	}
	return true;
}

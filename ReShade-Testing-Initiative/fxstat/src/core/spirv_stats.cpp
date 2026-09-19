#include "spirv_stats.hpp"

#include <vector>
#include <set>
#include <map>

namespace
{
	constexpr uint32_t k_spirv_magic = 0x07230203;

	constexpr uint16_t k_op_entry_point = 15;
	constexpr uint16_t k_op_function = 54;
	constexpr uint16_t k_op_function_end = 56;
	constexpr uint16_t k_op_function_call = 57;
	constexpr uint16_t k_op_function_parameter = 55;

	struct instruction
	{
		uint16_t opcode;
		const uint32_t *operands;
		uint16_t operand_count;
	};

	// Read the NUL-terminated UTF-8 literal that starts at `words[0]`.
	std::string read_literal_string(const uint32_t *words, uint16_t max_words)
	{
		std::string result;
		for (uint16_t w = 0; w < max_words; ++w)
		{
			for (int b = 0; b < 4; ++b)
			{
				const char c = static_cast<char>((words[w] >> (b * 8)) & 0xFF);
				if (c == '\0')
					return result;
				result += c;
			}
		}
		return result;
	}
}

bool fxstat::analyze_spirv(const uint32_t *words, size_t word_count, shader_stats &out,
	bool *inliner_incomplete, std::string &error)
{
	if (word_count < 5)
	{
		error = "SPIR-V module is too short to contain a header";
		return false;
	}
	if (words[0] != k_spirv_magic)
	{
		error = "not a SPIR-V module (bad magic word)";
		return false;
	}

	// -----------------------------------------------------------------------
	// Pass 1: index the module.
	//
	// A ReShade-generated module contains every function in the effect, with one
	// OpEntryPoint naming the one that matters. Counting the raw instruction
	// stream therefore counts code this entry point never calls -- including all
	// the other passes' shaders. So build the call graph and only count what is
	// actually reachable.
	// -----------------------------------------------------------------------

	std::vector<uint32_t> entry_function_ids;
	std::map<uint32_t, std::vector<size_t>> function_body;   // function id -> instruction indices
	std::map<uint32_t, std::vector<uint32_t>> function_calls; // function id -> callee ids
	std::vector<instruction> instructions;

	{
		size_t i = 5;
		uint32_t current_function = 0;

		while (i < word_count)
		{
			const uint32_t header = words[i];
			const uint16_t opcode = static_cast<uint16_t>(header & 0xFFFF);
			const uint16_t length = static_cast<uint16_t>(header >> 16);

			if (length == 0 || i + length > word_count)
			{
				error = "malformed instruction stream";
				return false;
			}

			const instruction inst { opcode, words + i + 1, static_cast<uint16_t>(length - 1) };

			if (opcode == k_op_entry_point && inst.operand_count >= 2)
			{
				// OpEntryPoint <execution model> <function id> <name...>
				entry_function_ids.push_back(inst.operands[1]);
			}
			else if (opcode == k_op_function && inst.operand_count >= 2)
			{
				// OpFunction <result type> <result id> <control> <function type>
				current_function = inst.operands[1];
				function_body[current_function];
				function_calls[current_function];
			}
			else if (opcode == k_op_function_end)
			{
				current_function = 0;
			}

			if (current_function != 0)
			{
				if (opcode == k_op_function_call && inst.operand_count >= 3)
					function_calls[current_function].push_back(inst.operands[2]);

				function_body[current_function].push_back(instructions.size());
			}

			instructions.push_back(inst);
			i += length;
		}
	}

	// -----------------------------------------------------------------------
	// Pass 2: walk the call graph from the entry points.
	// -----------------------------------------------------------------------

	std::set<uint32_t> reachable;
	{
		std::vector<uint32_t> worklist = entry_function_ids;
		// A module with no OpEntryPoint at all (unusual, but possible for a
		// library module) is counted in full rather than reported as empty.
		if (worklist.empty())
			for (const auto &[id, body] : function_body)
				worklist.push_back(id);

		while (!worklist.empty())
		{
			const uint32_t id = worklist.back();
			worklist.pop_back();
			if (!reachable.insert(id).second)
				continue;
			if (const auto it = function_calls.find(id); it != function_calls.end())
				for (const uint32_t callee : it->second)
					if (reachable.find(callee) == reachable.end())
						worklist.push_back(callee);
		}
	}

	// -----------------------------------------------------------------------
	// Pass 3: count.
	// -----------------------------------------------------------------------

	for (const uint32_t id : reachable)
	{
		const auto it = function_body.find(id);
		if (it == function_body.end())
			continue;

		out.functions++;

		for (const size_t index : it->second)
		{
			const instruction &inst = instructions[index];
			category c;
			if (!classify_opcode(inst.opcode, c))
				continue;
			out.counts[static_cast<size_t>(c)]++;
			out.total++;
			out.by_opcode[opcode_name(inst.opcode)]++;

			// After the driver pass list every function should have been inlined
			// into its entry point. A surviving parameter means the SPIRV-Tools
			// inliner declined this module, so mem2reg had nothing to work on and
			// the load/store scaffolding is still being counted.
			if (inst.opcode == k_op_function_parameter && inliner_incomplete != nullptr)
				*inliner_incomplete = true;
		}
	}

	return true;
}

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
	constexpr uint16_t k_op_ext_inst_import = 11;
	constexpr uint16_t k_op_ext_inst = 12;
	constexpr uint16_t k_op_type_vector = 23;
	constexpr uint16_t k_op_type_matrix = 24;
	constexpr uint16_t k_op_constant = 43;
	constexpr uint16_t k_op_constant_composite = 44;
	constexpr uint16_t k_op_fdiv = 136;

	// Transcendental cost of a GLSL.std.450 extended instruction, in hardware
	// transcendental operations (sin, cos, exp2, log2, rcp, sqrt, rsqrt). 0 for
	// everything that is plain ALU.
	uint32_t glsl_std_450_transcendentals(uint32_t instruction)
	{
		switch (instruction)
		{
		case 13: case 14:                     // Sin, Cos
		case 27: case 28: case 29: case 30:   // Exp, Log, Exp2, Log2
		case 31: case 32:                     // Sqrt, InverseSqrt
		case 66: case 67: case 69:            // Length, Distance, Normalize (one sqrt/rsqrt)
			return 1;
		case 26:                              // Pow = exp2(y * log2(x))
		case 16: case 17: case 18: case 25:   // Asin, Acos, Atan, Atan2: polynomial + rcp/sqrt
		case 19: case 20: case 21:            // Sinh, Cosh, Tanh: exp + rcp
			return 2;
		case 15:                              // Tan = sin / cos
			return 3;
		default:
			return 0;
		}
	}

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
	std::map<uint32_t, uint32_t> type_components; // type id -> scalar component count (absent = scalar)
	std::set<uint32_t> constants;                 // result ids of OpConstant / OpConstantComposite
	uint32_t glsl_std_450 = 0;                    // result id of the GLSL.std.450 import

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

			if (opcode == k_op_type_vector && inst.operand_count >= 3)
			{
				// OpTypeVector <result id> <component type> <count>
				type_components[inst.operands[0]] = inst.operands[2];
			}
			else if (opcode == k_op_type_matrix && inst.operand_count >= 3)
			{
				// OpTypeMatrix <result id> <column type> <column count>
				const auto col = type_components.find(inst.operands[1]);
				type_components[inst.operands[0]] = (col != type_components.end() ? col->second : 1) * inst.operands[2];
			}
			else if ((opcode == k_op_constant || opcode == k_op_constant_composite) && inst.operand_count >= 2)
			{
				constants.insert(inst.operands[1]);
			}
			else if (opcode == k_op_ext_inst_import && inst.operand_count >= 2)
			{
				if (read_literal_string(inst.operands + 1, inst.operand_count - 1) == "GLSL.std.450")
					glsl_std_450 = inst.operands[0];
			}
			else if (opcode == k_op_entry_point && inst.operand_count >= 2)
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

			if (c == category::alu && inst.operand_count >= 2)
			{
				// Value instructions start with <result type> <result id>.
				const auto t = type_components.find(inst.operands[0]);
				const uint32_t lanes = (t != type_components.end()) ? t->second : 1;
				out.alu_lanes += lanes;

				uint32_t trans = 0, trans_lanes = lanes;
				if (inst.opcode == k_op_ext_inst && inst.operand_count >= 4 && inst.operands[2] == glsl_std_450)
				{
					trans = glsl_std_450_transcendentals(inst.operands[3]);
					// Length and Distance return a scalar already; Normalize is one
					// rsqrt of the squared length followed by plain multiplies.
					if (inst.operands[3] == 69)
						trans_lanes = 1;
				}
				else if (inst.opcode == k_op_fdiv && inst.operand_count >= 4 &&
				         constants.find(inst.operands[3]) == constants.end())
				{
					// A division by a constant becomes a multiply; anything else
					// is a reciprocal (and a multiply) on every current GPU.
					trans = 1;
				}
				out.trans += trans;
				out.trans_lanes += trans * trans_lanes;
			}

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

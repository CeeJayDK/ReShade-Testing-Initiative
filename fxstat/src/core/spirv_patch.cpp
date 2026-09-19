/*
 * Bake specialisation constant values into a SPIR-V binary.
 *
 * ReShade's SPIR-V back end emits performance-mode uniforms as OpSpecConstant
 * and supplies the real values through VkSpecializationInfo at pipeline
 * creation -- the values never appear in the module. So a module taken straight
 * from the code generator always carries the values written in the effect
 * source, whatever the preset says, and any analysis of it is analysis of the
 * defaults.
 *
 * spirv-opt --set-spec-const-default-value is the obvious substitute but is not
 * sufficient: it silently ignores hex bit patterns (so floats have to round-trip
 * through decimal text) and it does not handle OpSpecConstantTrue/False at all,
 * so booleans -- the uniforms most likely to gate a whole branch -- cannot be
 * set with it. Patching the words directly avoids both problems.
 */

#include "spirv_patch.hpp"

#include <map>

namespace
{
	constexpr uint32_t k_spirv_magic = 0x07230203;

	constexpr uint16_t k_op_spec_constant_true = 48;
	constexpr uint16_t k_op_spec_constant_false = 49;
	constexpr uint16_t k_op_spec_constant = 50;
	constexpr uint16_t k_op_decorate = 71;

	constexpr uint32_t k_decoration_spec_id = 1;
}

bool fxstat::patch_spec_constants(std::string &binary,
	const std::vector<uint32_t> &values_by_spec_id, std::string &error)
{
	if (binary.size() % 4 != 0 || binary.size() < 20)
	{
		error = "SPIR-V binary is malformed";
		return false;
	}

	uint32_t *const words = reinterpret_cast<uint32_t *>(binary.data());
	const size_t word_count = binary.size() / 4;

	if (words[0] != k_spirv_magic)
	{
		error = "not a SPIR-V module (bad magic word)";
		return false;
	}

	// Pass 1: result id -> spec id, from the OpDecorate ... SpecId <n> section.
	std::map<uint32_t, uint32_t> spec_id_of;
	// Pass 2 rewrites the constants. Both passes walk the same stream.
	for (int pass = 0; pass < 2; ++pass)
	{
		size_t i = 5;
		while (i < word_count)
		{
			const uint16_t opcode = static_cast<uint16_t>(words[i] & 0xFFFF);
			const uint16_t length = static_cast<uint16_t>(words[i] >> 16);

			if (length == 0 || i + length > word_count)
			{
				error = "malformed instruction stream";
				return false;
			}

			if (pass == 0)
			{
				// OpDecorate <target> <decoration> [<literal>]
				if (opcode == k_op_decorate && length >= 4 && words[i + 2] == k_decoration_spec_id)
					spec_id_of[words[i + 1]] = words[i + 3];
			}
			else
			{
				uint32_t result_id = 0;
				if (opcode == k_op_spec_constant && length >= 4)
					result_id = words[i + 2];                    // <result type> <result id> <literal...>
				else if (opcode == k_op_spec_constant_true || opcode == k_op_spec_constant_false)
					result_id = (length >= 3) ? words[i + 2] : 0; // <result type> <result id>

				if (result_id != 0)
				{
					const auto it = spec_id_of.find(result_id);
					if (it != spec_id_of.end() && it->second < values_by_spec_id.size())
					{
						const uint32_t value = values_by_spec_id[it->second];

						if (opcode == k_op_spec_constant)
						{
							// Only 32-bit scalars are emitted as external
							// specialisation constants by ReShade's back end.
							if (length == 4)
								words[i + 3] = value;
						}
						else
						{
							// Both forms are exactly 3 words, so the opcode can be
							// swapped in place without moving anything.
							const uint16_t new_op = value ? k_op_spec_constant_true : k_op_spec_constant_false;
							words[i] = (static_cast<uint32_t>(length) << 16) | new_op;
						}
					}
				}
			}

			i += length;
		}
	}

	return true;
}

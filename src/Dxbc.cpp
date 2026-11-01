#include "Dxbc.h"

#include <Windows.h>

#include <algorithm>
#include <cstring>

#include "DxbcFormat.h"
#include "prologue/p_0_0_0.h"
#include "prologue/p_0_0_1.h"
#include "prologue/p_0_1_0.h"
#include "prologue/p_0_1_1.h"
#include "prologue/p_1_0_0.h"
#include "prologue/p_1_0_1.h"
#include "prologue/p_1_1_0.h"
#include "prologue/p_1_1_1.h"
#include "prologue/p_2_0_0.h"
#include "prologue/p_2_0_1.h"
#include "prologue/p_2_1_0.h"
#include "prologue/p_2_1_1.h"

namespace Pom::Dxbc
{
namespace
{
	constexpr uint32_t PROLOGUE_CONSTANTS = 12;
	constexpr uint32_t LITERAL_VECTORS = 3;
	constexpr uint32_t COLOR_RESOURCE = 0;
	constexpr uint32_t NORMAL_RESOURCE = 1;

	// The game's constants the shaders are patched around: PerSceneConsts (b0) and the per-object b3.
	constexpr uint32_t SCENE_BUFFER = 0;
	constexpr uint32_t OBJECT_BUFFER = 3;
	constexpr uint32_t SUN_DIFFUSE = 19;
	constexpr uint32_t PRIMARY_DIFFUSE = 78;
	constexpr uint32_t SPOT_LOOKUP_FIRST = 22;  // cb0[22..24] with the position, then cb0[25].
	constexpr uint32_t SPOT_LOOKUP_LAST = 24;
	constexpr uint32_t LIGHT_FLAGS = 11;
	constexpr uint32_t SCENE_VECTORS_READ = 79;  // What the prologue reads of b0.
	constexpr uint32_t OMNI_FLAG = 1;

	// The prologue's outputs, as SV_Target indices in Prologue.hlsl.
	enum : uint32_t
	{
		OUT_UV,
		OUT_UV_DDX,
		OUT_UV_DDY,
		OUT_SHADE,
		OUT_LIGHT_COLOR,
		OUT_LOOKUP,
		OUT_POSITION,
		OUT_COUNT,
	};

	// Components of OUT_SHADE.
	constexpr uint32_t SHADE_CREVICE = 0;
	constexpr uint32_t SHADE_SPOT = 2;
	constexpr uint32_t SHADE_OMNI = 3;

	// The prologue's inputs, as registers in Prologue.hlsl.
	enum : uint32_t
	{
		IN_UV,
		IN_NORMAL,
		IN_POSITION,
		IN_LOOKUP,
		IN_COUNT,
	};

	struct Variant
	{
		const BYTE* code;
		size_t size;
	};

	// By light, then twin, then moved lookup.
	const Variant VARIANTS[3][2][2] = {
		{{{g_Prologue_0_0_0, sizeof(g_Prologue_0_0_0)}, {g_Prologue_0_0_1, sizeof(g_Prologue_0_0_1)}},
			{{g_Prologue_0_1_0, sizeof(g_Prologue_0_1_0)}, {g_Prologue_0_1_1, sizeof(g_Prologue_0_1_1)}}},
		{{{g_Prologue_1_0_0, sizeof(g_Prologue_1_0_0)}, {g_Prologue_1_0_1, sizeof(g_Prologue_1_0_1)}},
			{{g_Prologue_1_1_0, sizeof(g_Prologue_1_1_0)}, {g_Prologue_1_1_1, sizeof(g_Prologue_1_1_1)}}},
		{{{g_Prologue_2_0_0, sizeof(g_Prologue_2_0_0)}, {g_Prologue_2_0_1, sizeof(g_Prologue_2_0_1)}},
			{{g_Prologue_2_1_0, sizeof(g_Prologue_2_1_0)}, {g_Prologue_2_1_1, sizeof(g_Prologue_2_1_1)}}},
	};

	struct Registers
	{
		uint32_t input[IN_COUNT] = {};    // The game's registers for the prologue's inputs.
		uint32_t constants = 0;           // Temps holding the literals: the prologue's cb12.
		uint32_t output[OUT_COUNT] = {};  // Temps for the prologue's outputs.
		uint32_t temps = 0;               // Where the prologue's r0 goes.
	};

	bool Reads(const Operand& o, uint32_t buffer, uint32_t vector)
	{
		return o.type == OPERAND_CONSTANT_BUFFER && o.dimension == 2 && o.immediateIndices && o.index0 == buffer && o.index1 == vector;
	}

	int EndOfBlock(const std::vector<Instruction>& code, int open)
	{
		int depth = 0;
		for (int i = open; i < int(code.size()); ++i)
		{
			if (code[i].opcode == OP_IF)
				++depth;
			else if (code[i].opcode == OP_ENDIF && --depth == 0)
				return i;
		}
		return -1;
	}

	// What's in a shader that the prologue has to do something about.
	struct Features
	{
		uint32_t light = 0;         // 0 none, 1 the sun, 2 a primary light.
		bool readsLookup = false;
		int spotEnd = -1;           // The endif of the placed spot's block, if the block is what it should be.
		Operand spotResult;
		int omniMovc = -1;          // The movc behind the omnis' block, which picks up what the block added up.
		Operand omniResult;
	};

	Features FindFeatures(const std::vector<uint32_t>& words, const std::vector<Instruction>& code, int firstCode, int lookupInput)
	{
		Features features;
		int spotOpen = -1, omniOpen = -1;
		std::vector<Operand> operands;
		for (int i = firstCode; i < int(code.size()); ++i)
		{
			if (!OperandsOf(words, code[i], operands))
				continue;
			for (const Operand& o : operands)
			{
				if (Reads(o, SCENE_BUFFER, SUN_DIFFUSE))
					features.light = 1;
				else if (Reads(o, SCENE_BUFFER, PRIMARY_DIFFUSE))
					features.light = 2;
				else if (o.type == OPERAND_INPUT && lookupInput >= 0 && int(o.index0) == lookupInput)
					features.readsLookup = true;
				else if (code[i].opcode == OP_IF && Reads(o, OBJECT_BUFFER, LIGHT_FLAGS))
					((o.selection & 3) == OMNI_FLAG ? omniOpen : spotOpen) = i;
			}
		}

		// if_nz cb3[11].x  <spot>  else  mov rOut.xyz, l(0,0,0,0)  endif
		const int spotEnd = spotOpen >= 0 ? EndOfBlock(code, spotOpen) : -1;
		if (spotEnd >= 2 && code[spotEnd - 2].opcode == OP_ELSE && code[spotEnd - 1].opcode == OP_MOV && OperandsOf(words, code[spotEnd - 1], operands) &&
			!operands.empty() && operands[0].type == OPERAND_TEMP && operands[0].mode == MODE_MASK)
		{
			features.spotEnd = spotEnd;
			features.spotResult = operands[0];
		}
		// if_nz cb3[11].y  <omnis>  endif  movc rA.xyz, cb3[11].yyyy, rB.xyzx, l(0,0,0,0)
		const int omniEnd = omniOpen >= 0 ? EndOfBlock(code, omniOpen) : -1;
		if (omniEnd >= 0 && omniEnd + 1 < int(code.size()) && code[omniEnd + 1].opcode == OP_MOVC && OperandsOf(words, code[omniEnd + 1], operands) &&
			!operands.empty() && operands[0].type == OPERAND_TEMP && operands[0].mode == MODE_MASK)
		{
			features.omniMovc = omniEnd + 1;
			features.omniResult = operands[0];
		}
		return features;
	}

	// mul rN.mask, rN, rFactor.cccc
	void AppendScale(std::vector<uint32_t>& out, uint32_t reg, uint32_t mask, uint32_t factor, uint32_t component)
	{
		out.push_back(OP_MUL | (7u << 24));
		out.push_back(TempToken(MODE_MASK, mask));
		out.push_back(reg);
		out.push_back(TempToken(MODE_SWIZZLE, SWIZZLE_XYZW));
		out.push_back(reg);
		out.push_back(TempToken(MODE_SWIZZLE, component * 0x55));
		out.push_back(factor);
	}

	// Copies an instruction of the prologue, with its registers moved to where they went in the game's shader.
	bool AppendPrologueInstruction(const std::vector<uint32_t>& words, const Instruction& ins, const Registers& regs,
		std::vector<uint32_t>& out, std::string& error)
	{
		std::vector<Operand> operands;
		if (!OperandsOf(words, ins, operands))
		{
			error = "unreadable prologue";
			return false;
		}
		const size_t start = out.size();
		out.insert(out.end(), words.begin() + ins.at, words.begin() + ins.operandsAt);
		for (const Operand& o : operands)
		{
			const auto copyAll = [&] { out.insert(out.end(), words.begin() + o.at, words.begin() + o.at + o.size); };
			// The token, any extended tokens, and one index in place of what the operand had.
			const auto retarget = [&](uint32_t token, uint32_t index)
			{
				out.push_back(token);
				out.insert(out.end(), words.begin() + o.at + 1, words.begin() + o.indexAt);
				out.push_back(index);
			};
			switch (o.type)
			{
			case OPERAND_TEMP:
				retarget(o.token, regs.temps + o.index0);
				break;
			case OPERAND_INPUT:
				if (o.index0 >= IN_COUNT)
				{
					error = "prologue reads an unknown input";
					return false;
				}
				retarget(o.token, regs.input[o.index0]);
				break;
			case OPERAND_OUTPUT:
			{
				const uint32_t token = (o.token & ~(0xFFu << 12)) | (OPERAND_TEMP << 12);
				if (o.index0 >= OUT_COUNT)
				{
					error = "prologue writes an unknown output";
					return false;
				}
				retarget(token, regs.output[o.index0]);
				break;
			}
			case OPERAND_CONSTANT_BUFFER:
			{
				// b0 and b3 are the game's, and bound. Only the literals become something else.
				if (o.index0 != PROLOGUE_CONSTANTS)
				{
					copyAll();
					break;
				}
				if (o.index1 >= LITERAL_VECTORS || !o.immediateIndices)
				{
					error = "prologue reads an unknown constant";
					return false;
				}
				const uint32_t token = (o.token & ~((0xFFu << 12) | (3u << 20))) | (OPERAND_TEMP << 12) | (1u << 20);
				retarget(token, regs.constants + o.index1);
				break;
			}
			default:
				copyAll();
			}
		}
		out[start] = WithLength(out[start], out.size() - start);
		return true;
	}

	void AppendLiteral(std::vector<uint32_t>& out, uint32_t reg, const float values[4])
	{
		out.push_back(OP_MOV | (8u << 24));
		out.push_back(TempToken(MODE_MASK, MASK_XYZW));
		out.push_back(reg);
		out.push_back(2 | (OPERAND_IMMEDIATE32 << 12));
		for (int i = 0; i < 4; ++i)
		{
			uint32_t bits;
			memcpy(&bits, &values[i], 4);
			out.push_back(bits);
		}
	}

	// sample dest, vUv.xyxx, tn, sn  ->  sample_d dest, rUv.xyxx, tn, sn, rUvDdx.xyxx, rUvDdy.xyxx
	bool IsLayer0Sample(const Instruction& ins, const std::vector<Operand>& operands, uint32_t uvInput)
	{
		if (ins.opcode != OP_SAMPLE || operands.size() != 4)
			return false;
		const Operand& address = operands[1];
		return address.type == OPERAND_INPUT && address.index0 == uvInput && address.mode == MODE_SWIZZLE &&
			(address.selection & 15) == (0 | (1 << 2));
	}

	void AppendLayer0Sample(const std::vector<uint32_t>& words, const Instruction& ins, const std::vector<Operand>& operands,
		const Registers& regs, std::vector<uint32_t>& out)
	{
		const size_t start = out.size();
		out.insert(out.end(), words.begin() + ins.at, words.begin() + ins.operandsAt);
		out[start] = (out[start] & ~0x7FFu) | OP_SAMPLE_D;
		const auto copyOperand = [&](const Operand& o) { out.insert(out.end(), words.begin() + o.at, words.begin() + o.at + o.size); };
		const auto appendTemp = [&](uint32_t reg)
		{
			out.push_back(TempToken(MODE_SWIZZLE, SWIZZLE_XYXX));
			out.push_back(reg);
		};
		copyOperand(operands[0]);
		appendTemp(regs.output[OUT_UV]);
		copyOperand(operands[2]);
		copyOperand(operands[3]);
		appendTemp(regs.output[OUT_UV_DDX]);
		appendTemp(regs.output[OUT_UV_DDY]);
		out[start] = WithLength(out[start], out.size() - start);
		if (operands[2].type == OPERAND_RESOURCE && operands[2].index0 == COLOR_RESOURCE && operands[0].type == OPERAND_TEMP)
			AppendScale(out, operands[0].index0, operands[0].selection & MASK_XYZ, regs.output[OUT_SHADE], SHADE_CREVICE);
	}

	bool StaysWithinUv(const Operand& o)
	{
		if (o.mode == MODE_SELECT1)
			return (o.selection & 3) < 2;
		return o.mode == MODE_SWIZZLE && !((o.selection >> 1) & 1) && !((o.selection >> 3) & 1) && !((o.selection >> 5) & 1) && !((o.selection >> 7) & 1);
	}

	// One of the game's instructions, reading what the prologue worked out where it should: layer 0's UV (reads of the
	// lightmap UV in the same register stay as they are), the light's color, and the position shadows are looked up at.
	void AppendGameInstruction(const std::vector<uint32_t>& words, const Instruction& ins, const std::vector<Operand>& operands, const Registers& regs,
		const Features& features, bool movedLookup, std::vector<uint32_t>& out)
	{
		if (ins.opcode == OP_CUSTOMDATA)
		{
			out.insert(out.end(), words.begin() + ins.at, words.begin() + ins.at + ins.length);
			return;
		}
		const bool spotLookup = std::any_of(operands.begin(), operands.end(), [](const Operand& o)
		{
			return o.type == OPERAND_CONSTANT_BUFFER && o.index0 == SCENE_BUFFER && o.index1 >= SPOT_LOOKUP_FIRST && o.index1 <= SPOT_LOOKUP_LAST;
		});
		const uint32_t lightVector = features.light == 1 ? SUN_DIFFUSE : PRIMARY_DIFFUSE;

		const size_t start = out.size();
		out.insert(out.end(), words.begin() + ins.at, words.begin() + ins.operandsAt);
		for (const Operand& o : operands)
		{
			const auto retarget = [&](uint32_t reg)
			{
				out.push_back((o.token & ~((0xFFu << 12) | (3u << 20))) | (OPERAND_TEMP << 12) | (1u << 20));
				out.insert(out.end(), words.begin() + o.at + 1, words.begin() + o.indexAt);
				out.push_back(reg);
			};
			if (o.type == OPERAND_INPUT && o.immediateIndices && o.index0 == regs.input[IN_UV] && StaysWithinUv(o))
				retarget(regs.output[OUT_UV]);
			else if (movedLookup && o.type == OPERAND_INPUT && o.immediateIndices && o.index0 == regs.input[IN_LOOKUP])
				retarget(regs.output[OUT_LOOKUP]);
			else if (spotLookup && features.spotEnd >= 0 && o.type == OPERAND_INPUT && o.immediateIndices && o.index0 == regs.input[IN_POSITION])
				retarget(regs.output[OUT_POSITION]);
			else if (features.light && Reads(o, SCENE_BUFFER, lightVector))
				retarget(regs.output[OUT_LIGHT_COLOR]);
			else
				out.insert(out.end(), words.begin() + o.at, words.begin() + o.at + o.size);
		}
		out[start] = WithLength(out[start], out.size() - start);
	}

}

Result Patch(const uint8_t* shader, size_t size, const Literals& literals)
{
	Result result;
	std::vector<Chunk> chunks;
	std::vector<uint32_t> words;
	std::vector<Instruction> code;
	if (!ReadChunks(shader, size, chunks))
	{
		result.error = "not a shader container";
		return result;
	}
	if (!ReadProgram(chunks, words) || !ReadInstructions(words, code))
	{
		result.error = "unreadable program";
		return result;
	}
	if (((words[0] >> 16) & 0xFFFF) != 0 || ((words[0] >> 4) & 15) != 4)
	{
		result.error = "not a shader model 4 pixel shader";
		return result;
	}

	const auto signature = std::find_if(chunks.begin(), chunks.end(), [](const Chunk& chunk) { return chunk.tag == TAG_ISGN; });
	if (signature == chunks.end())
	{
		result.error = "no input signature";
		return result;
	}
	// A copy, since what the shader reads goes up.
	std::vector<uint8_t> inputSignature(signature->data, signature->data + signature->size);
	signature->data = inputSignature.data();

	int firstCode = -1, temps = -1;
	for (int i = 0; i < int(code.size()); ++i)
	{
		if (code[i].opcode == OP_DCL_TEMPS)
			temps = i;
		if (!IsDeclaration(code[i].opcode))
		{
			firstCode = i;
			break;
		}
	}
	if (temps < 0 || firstCode < 0)
	{
		result.error = "no dcl_temps";
		return result;
	}

	// TEXCOORD4 is the shadow lookup wherever a shader reads all of it: the vertex shaders make it from cb0[48..51].
	const int lookupEntry = FindTexcoord(*signature, 4);
	const bool wholeLookup = lookupEntry >= 0 && (inputSignature[lookupEntry + SIGNATURE_MASK] & MASK_XYZW) == MASK_XYZW;
	const int lookupInput = wholeLookup ? int(ReadU32(inputSignature.data() + lookupEntry + SIGNATURE_REGISTER)) : -1;
	const Features features = FindFeatures(words, code, firstCode, lookupInput);
	const bool twin = features.spotEnd >= 0 || features.omniMovc >= 0;
	result.light = features.light;
	result.movedLookup = features.readsLookup;
	result.spotBlock = features.spotEnd >= 0;
	result.omniBlock = features.omniMovc >= 0;

	const Variant& variant = VARIANTS[features.light][twin ? 1 : 0][features.readsLookup ? 1 : 0];
	std::vector<Chunk> prologueChunks;
	std::vector<uint32_t> prologueWords;
	std::vector<Instruction> prologue;
	if (!ReadChunks(variant.code, variant.size, prologueChunks) || !ReadProgram(prologueChunks, prologueWords) ||
		!ReadInstructions(prologueWords, prologue))
	{
		result.error = "unreadable prologue";
		return result;
	}

	// TEXCOORD0 = uv, TEXCOORD1 = normal, TEXCOORD5 = camera to pixel, TEXCOORD4 = shadow lookup: the prologue's inputs.
	const uint32_t semantics[IN_COUNT] = {0, 1, 5, 4};
	const uint32_t masks[IN_COUNT] = {MASK_XY, MASK_XYZ, MASK_XYZ, MASK_XYZW};
	const int inputCount = features.readsLookup ? IN_COUNT : IN_LOOKUP;
	Registers regs;
	struct NewInput
	{
		size_t at;
		uint32_t reg;
		uint32_t mask;
	};
	std::vector<NewInput> newInputs;
	for (int i = 0; i < inputCount; ++i)
	{
		const int entry = FindTexcoord(*signature, semantics[i]);
		if (entry < 0 || (inputSignature[entry + SIGNATURE_MASK] & masks[i]) != masks[i])
		{
			result.error = "the shader isn't sent uv, normal and view position";
			return result;
		}
		regs.input[i] = ReadU32(inputSignature.data() + entry + SIGNATURE_REGISTER);
		inputSignature[entry + SIGNATURE_USED] |= uint8_t(masks[i]);
		if (const size_t at = DeclareInput(words, code, regs.input[i], masks[i]))
			newInputs.push_back({at, regs.input[i], masks[i]});
	}
	std::sort(newInputs.begin(), newInputs.end(), [](const NewInput& a, const NewInput& b) { return a.at < b.at; });
	WidenConstants(words, code, SCENE_BUFFER, SCENE_VECTORS_READ);

	uint32_t prologueTemps = 0;
	for (const Instruction& ins : prologue)
	{
		if (ins.opcode == OP_DCL_TEMPS)
			prologueTemps = prologueWords[ins.at + 1];
	}

	const uint32_t gameTemps = words[code[temps].at + 1];
	// The prologue runs before any of the game's code, so its working registers can be the game's own: nothing is in them
	// yet. Only what it hands over needs a place of its own. That keeps the count down, and MoreLights takes 32 at most.
	const uint32_t shared = (std::max)(gameTemps, prologueTemps + LITERAL_VECTORS);
	regs.temps = 0;
	regs.constants = prologueTemps;
	for (uint32_t i = 0; i < OUT_COUNT; ++i)
		regs.output[i] = shared + i;

	std::vector<uint32_t> out;
	out.reserve(words.size() + prologueWords.size() + 64);
	size_t copied = 0;
	for (const NewInput& input : newInputs)
	{
		out.insert(out.end(), words.begin() + copied, words.begin() + input.at);
		out.push_back(OP_DCL_INPUT_PS | INTERPOLATION_LINEAR | (3u << 24));
		out.push_back(2 | (MODE_MASK << 2) | (input.mask << 4) | (OPERAND_INPUT << 12) | (1u << 20));
		out.push_back(input.reg);
		copied = input.at;
	}
	out.insert(out.end(), words.begin() + copied, words.begin() + code[firstCode].at);
	// The new declarations are all in front of dcl_temps, and each is three words.
	const size_t tempsAt = code[temps].at + 1 + newInputs.size() * 3;
	out[tempsAt] = shared + OUT_COUNT;

	const float* values = reinterpret_cast<const float*>(&literals);
	for (uint32_t i = 0; i < LITERAL_VECTORS; ++i)
		AppendLiteral(out, regs.constants + i, values + i * 4);
	for (const Instruction& ins : prologue)
	{
		if (IsDeclaration(ins.opcode) || ins.opcode == OP_RET)
			continue;
		if (!AppendPrologueInstruction(prologueWords, ins, regs, out, result.error))
			return result;
	}

	bool sawColor = false, sawNormal = false;
	for (int i = firstCode; i < int(code.size()); ++i)
	{
		const Instruction& ins = code[i];
		std::vector<Operand> operands;
		if (!OperandsOf(words, ins, operands))
		{
			result.error = "unreadable instruction";
			return result;
		}
		if (IsLayer0Sample(ins, operands, regs.input[0]))
		{
			AppendLayer0Sample(words, ins, operands, regs, out);
			++result.samples;
			sawColor |= operands[2].type == OPERAND_RESOURCE && operands[2].index0 == COLOR_RESOURCE;
			sawNormal |= operands[2].type == OPERAND_RESOURCE && operands[2].index0 == NORMAL_RESOURCE;
			continue;
		}
		AppendGameInstruction(words, ins, operands, regs, features, features.readsLookup, out);
		if (i == features.spotEnd)
			AppendScale(out, features.spotResult.index0, features.spotResult.selection & MASK_XYZW, regs.output[OUT_SHADE], SHADE_SPOT);
		if (i == features.omniMovc)
			AppendScale(out, features.omniResult.index0, features.omniResult.selection & MASK_XYZW, regs.output[OUT_SHADE], SHADE_OMNI);
	}
	if (!sawColor || !sawNormal)
	{
		result.error = "the shader doesn't sample color and normal at the material's uv";
		return result;
	}

	out[1] = uint32_t(out.size());
	result.shader = BuildContainer(chunks, out);
	result.ok = true;
	return result;
}
}

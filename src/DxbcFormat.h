#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// Reading and writing compiled shaders: the container, its signatures, and the tokens of the program. Both patches use
// this: Dxbc.cpp (parallax) and Brdf.cpp (highlights and reflections).
namespace Pom::Dxbc
{
	// Opcodes and operand encoding: d3d10TokenizedProgramFormat.hpp.
	enum Opcode : uint32_t
	{
		OP_ADD = 0,
		OP_DIV = 14,
		OP_DP3 = 16,
		OP_ELSE = 18,
		OP_ENDIF = 21,
		OP_EXP = 25,
		OP_IF = 31,
		OP_LOG = 47,
		OP_MAD = 50,
		OP_CUSTOMDATA = 53,
		OP_MOV = 54,
		OP_MOVC = 55,
		OP_MUL = 56,
		OP_RET = 62,
		OP_SAMPLE = 69,
		OP_SAMPLE_L = 72,
		OP_SAMPLE_D = 73,
		OP_DCL_RESOURCE = 88,
		OP_DCL_CONSTANT_BUFFER = 89,
		OP_DCL_SAMPLER = 90,
		OP_DCL_INPUT_PS = 98,
		OP_DCL_OUTPUT = 101,
		OP_DCL_TEMPS = 104,
		OP_DCL_GLOBAL_FLAGS = 106,
	};

	enum OperandType : uint32_t
	{
		OPERAND_TEMP = 0,
		OPERAND_INPUT = 1,
		OPERAND_OUTPUT = 2,
		OPERAND_IMMEDIATE32 = 4,
		OPERAND_IMMEDIATE64 = 5,
		OPERAND_SAMPLER = 6,
		OPERAND_RESOURCE = 7,
		OPERAND_CONSTANT_BUFFER = 8,
	};

	constexpr uint32_t MODE_MASK = 0;
	constexpr uint32_t MODE_SWIZZLE = 1;
	constexpr uint32_t MODE_SELECT1 = 2;
	constexpr uint32_t SWIZZLE_XYXX = 0 | (1 << 2);
	constexpr uint32_t SWIZZLE_XYZW = 0 | (1 << 2) | (2 << 4) | (3 << 6);
	constexpr uint32_t MASK_XY = 3;
	constexpr uint32_t MASK_XYZ = 7;
	constexpr uint32_t MASK_XYZW = 15;
	constexpr uint32_t INTERPOLATION_LINEAR = 2u << 11;
	constexpr uint32_t SATURATE = 1u << 13;

	constexpr uint32_t OpcodeOf(uint32_t token)
	{
		return token & 0x7FF;
	}

	constexpr uint32_t LengthOf(uint32_t token)
	{
		return (token >> 24) & 0x7F;
	}

	constexpr uint32_t WithLength(uint32_t token, size_t length)
	{
		return (token & ~(0x7Fu << 24)) | (uint32_t(length) << 24);
	}

	constexpr bool IsDeclaration(uint32_t opcode)
	{
		return opcode == OP_CUSTOMDATA || (opcode >= OP_DCL_RESOURCE && opcode <= OP_DCL_GLOBAL_FLAGS);
	}

	constexpr uint32_t TempToken(uint32_t mode, uint32_t selection)
	{
		return 2 | (mode << 2) | (selection << 4) | (OPERAND_TEMP << 12) | (1u << 20);
	}

	// One parsed operand: `at` is its first token, `size` its length in dwords.
	struct Operand
	{
		size_t at = 0;
		size_t size = 0;
		uint32_t token = 0;
		uint32_t type = 0;
		uint32_t mode = 0;
		uint32_t selection = 0;  // Mask or swizzle, per mode.
		size_t indexAt = 0;      // Position of the first index.
		uint32_t dimension = 0;
		bool immediateIndices = true;
		uint32_t index0 = 0, index1 = 0;
	};

	struct Instruction
	{
		size_t at = 0;
		size_t length = 0;
		uint32_t opcode = 0;
		size_t operandsAt = 0;
	};

	struct Chunk
	{
		uint32_t tag;
		const uint8_t* data;
		uint32_t size;
	};

	constexpr uint32_t FourCC(char a, char b, char c, char d)
	{
		return uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) | (uint32_t(uint8_t(c)) << 16) | (uint32_t(uint8_t(d)) << 24);
	}

	constexpr uint32_t TAG_SHDR = FourCC('S', 'H', 'D', 'R');
	constexpr uint32_t TAG_ISGN = FourCC('I', 'S', 'G', 'N');
	constexpr uint32_t TAG_OSGN = FourCC('O', 'S', 'G', 'N');

	constexpr size_t SIGNATURE_ENTRY = 24;
	constexpr size_t SIGNATURE_REGISTER = 16;
	constexpr size_t SIGNATURE_MASK = 20;
	constexpr size_t SIGNATURE_USED = 21;

	bool ReadOperand(const std::vector<uint32_t>& words, size_t at, size_t end, Operand& out);
	bool ReadInstructions(const std::vector<uint32_t>& words, std::vector<Instruction>& out);
	bool OperandsOf(const std::vector<uint32_t>& words, const Instruction& ins, std::vector<Operand>& out);

	uint32_t ReadU32(const uint8_t* p);
	bool ReadChunks(const uint8_t* shader, size_t size, std::vector<Chunk>& chunks);
	bool ReadProgram(const std::vector<Chunk>& chunks, std::vector<uint32_t>& words);

	// Where a TEXCOORD input sits in the signature, or -1. Its byte offset in the chunk.
	int FindTexcoord(const Chunk& signature, uint32_t semanticIndex);

	// The game's shaders leave out what they don't use: without specular, nothing reads the view position, though the
	// vertex shader still sends it. So an input may need declaring, or a declaration widening. Returns where a new
	// declaration goes in `words`, with `words` already widened if one existed, or 0 if none is needed.
	size_t DeclareInput(std::vector<uint32_t>& words, const std::vector<Instruction>& code, uint32_t input, uint32_t mask);

	// Raises what a shader declares of a constant buffer to at least `vectors`. The buffer behind it is the same size for
	// all of them.
	void WidenConstants(std::vector<uint32_t>& words, const std::vector<Instruction>& code, uint32_t buffer, uint32_t vectors);

	// A container with the signatures of `chunks` and `program` as its code. Reflection and statistics would be stale, and
	// the runtime only needs the rest.
	std::vector<uint8_t> BuildContainer(const std::vector<Chunk>& chunks, const std::vector<uint32_t>& program);
}

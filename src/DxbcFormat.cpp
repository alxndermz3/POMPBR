#include "DxbcFormat.h"

#include <cmath>
#include <cstring>

namespace Pom::Dxbc
{
namespace
{
	// The container hash: MD5's rounds with its own final block layout (DirectXShaderCompiler, DxilHash.cpp). The
	// runtime refuses shaders whose hash doesn't match.
	void Md5Block(uint32_t state[4], const uint8_t block[64])
	{
		static const int shifts[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14,
			20, 5, 9, 14, 20, 5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6,
			10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
		static uint32_t sines[64];
		static const bool initialised = []
		{
			for (int i = 0; i < 64; ++i)
				sines[i] = uint32_t(std::fabs(std::sin(double(i + 1))) * 4294967296.0);
			return true;
		}();
		(void)initialised;

		uint32_t words[16];
		memcpy(words, block, 64);
		uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
		for (int i = 0; i < 64; ++i)
		{
			uint32_t value;
			int index;
			if (i < 16)
			{
				value = (b & c) | (~b & d);
				index = i;
			}
			else if (i < 32)
			{
				value = (d & b) | (~d & c);
				index = (5 * i + 1) % 16;
			}
			else if (i < 48)
			{
				value = b ^ c ^ d;
				index = (3 * i + 5) % 16;
			}
			else
			{
				value = c ^ (b | ~d);
				index = (7 * i) % 16;
			}
			value += a + sines[i] + words[index];
			value = (value << shifts[i]) | (value >> (32 - shifts[i]));
			a = d;
			d = c;
			c = b;
			b += value;
		}
		state[0] += a;
		state[1] += b;
		state[2] += c;
		state[3] += d;
	}

	void RetailHash(const uint8_t* data, size_t length, uint8_t hash[16])
	{
		uint32_t state[4] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476};
		const size_t whole = length & ~size_t(63);
		for (size_t i = 0; i < whole; i += 64)
			Md5Block(state, data + i);
		const size_t tail = length - whole;
		const uint32_t bits = uint32_t(length) * 8;
		const uint32_t suffix = (uint32_t(length) * 2) | 1;
		uint8_t block[64] = {};
		if (tail < 56)
		{
			memcpy(block, &bits, 4);
			memcpy(block + 4, data + whole, tail);
			block[4 + tail] = 0x80;
			memcpy(block + 60, &suffix, 4);
			Md5Block(state, block);
		}
		else
		{
			memcpy(block, data + whole, tail);
			block[tail] = 0x80;
			Md5Block(state, block);
			memset(block, 0, sizeof(block));
			memcpy(block, &bits, 4);
			memcpy(block + 60, &suffix, 4);
			Md5Block(state, block);
		}
		memcpy(hash, state, 16);
	}
}

bool ReadOperand(const std::vector<uint32_t>& words, size_t at, size_t end, Operand& out)
{
	if (at >= end)
		return false;
	out = Operand();
	out.at = at;
	out.token = words[at];
	const uint32_t components = out.token & 3;
	out.mode = (out.token >> 2) & 3;
	out.type = (out.token >> 12) & 0xFF;
	out.dimension = (out.token >> 20) & 3;
	out.selection = components == 2 ? (out.token >> 4) & 0xFF : 0;
	size_t p = at + 1;
	for (uint32_t extended = out.token >> 31; extended;)
	{
		if (p >= end)
			return false;
		extended = words[p++] >> 31;
	}
	out.indexAt = p;
	for (uint32_t i = 0; i < out.dimension; ++i)
	{
		const uint32_t representation = (out.token >> (22 + i * 3)) & 7;
		switch (representation)
		{
		case 0:  // 32-bit immediate
			if (p >= end)
				return false;
			(i == 0 ? out.index0 : out.index1) = words[p++];
			break;
		case 1:  // 64-bit immediate
			p += 2;
			out.immediateIndices = false;
			break;
		case 2:  // Relative to a register
		case 3:  // 32-bit immediate + register
		case 4:  // 64-bit immediate + register
		{
			out.immediateIndices = false;
			p += representation == 3 ? 1 : representation == 4 ? 2 : 0;
			Operand relative;
			if (!ReadOperand(words, p, end, relative))
				return false;
			p += relative.size;
			break;
		}
		default:
			return false;
		}
	}
	if (out.type == OPERAND_IMMEDIATE32 || out.type == OPERAND_IMMEDIATE64)
	{
		const size_t count = components == 2 ? 4 : components == 1 ? 1 : 0;
		p += count * (out.type == OPERAND_IMMEDIATE64 ? 2 : 1);
	}
	out.size = p - at;
	return p <= end;
}

bool ReadInstructions(const std::vector<uint32_t>& words, std::vector<Instruction>& out)
{
	for (size_t p = 2; p < words.size();)
	{
		Instruction ins;
		ins.at = p;
		ins.opcode = OpcodeOf(words[p]);
		if (ins.opcode == OP_CUSTOMDATA)
		{
			if (p + 1 >= words.size())
				return false;
			ins.length = words[p + 1];
			ins.operandsAt = p + ins.length;
		}
		else
		{
			ins.length = LengthOf(words[p]);
			ins.operandsAt = p + 1;
			for (uint32_t extended = words[p] >> 31; extended;)
			{
				if (ins.operandsAt >= p + ins.length)
					return false;
				extended = words[ins.operandsAt++] >> 31;
			}
		}
		if (!ins.length || p + ins.length > words.size())
			return false;
		out.push_back(ins);
		p += ins.length;
	}
	return !out.empty();
}

bool OperandsOf(const std::vector<uint32_t>& words, const Instruction& ins, std::vector<Operand>& out)
{
	out.clear();
	const size_t end = ins.at + ins.length;
	for (size_t p = ins.operandsAt; p < end;)
	{
		Operand operand;
		if (!ReadOperand(words, p, end, operand) || !operand.size)
			return false;
		out.push_back(operand);
		p += operand.size;
	}
	return true;
}

uint32_t ReadU32(const uint8_t* p)
{
	uint32_t value;
	memcpy(&value, p, 4);
	return value;
}

bool ReadChunks(const uint8_t* shader, size_t size, std::vector<Chunk>& chunks)
{
	if (size < 32 || memcmp(shader, "DXBC", 4) != 0 || ReadU32(shader + 24) != size)
		return false;
	const uint32_t count = ReadU32(shader + 28);
	if (count > 64 || 32 + size_t(count) * 4 > size)
		return false;
	for (uint32_t i = 0; i < count; ++i)
	{
		const uint32_t offset = ReadU32(shader + 32 + i * 4);
		if (size_t(offset) + 8 > size)
			return false;
		const uint32_t chunkSize = ReadU32(shader + offset + 4);
		if (size_t(offset) + 8 + chunkSize > size)
			return false;
		chunks.push_back({ReadU32(shader + offset), shader + offset + 8, chunkSize});
	}
	return true;
}

bool ReadProgram(const std::vector<Chunk>& chunks, std::vector<uint32_t>& words)
{
	for (const Chunk& chunk : chunks)
	{
		if (chunk.tag != TAG_SHDR)
			continue;
		if (chunk.size < 8 || chunk.size % 4 != 0)
			return false;
		words.resize(chunk.size / 4);
		memcpy(words.data(), chunk.data, chunk.size);
		return words[1] == words.size();
	}
	return false;
}

int FindTexcoord(const Chunk& signature, uint32_t semanticIndex)
{
	const uint32_t count = signature.size >= 8 ? ReadU32(signature.data) : 0;
	for (uint32_t i = 0; i < count && 8 + (i + 1) * SIGNATURE_ENTRY <= signature.size; ++i)
	{
		const size_t at = 8 + i * SIGNATURE_ENTRY;
		const uint32_t name = ReadU32(signature.data + at);
		if (name < signature.size && strncmp(reinterpret_cast<const char*>(signature.data) + name, "TEXCOORD",
				signature.size - name) == 0 && ReadU32(signature.data + at + 4) == semanticIndex)
			return int(at);
	}
	return -1;
}

size_t DeclareInput(std::vector<uint32_t>& words, const std::vector<Instruction>& code, uint32_t input, uint32_t mask)
{
	size_t insertAt = 0;
	for (const Instruction& ins : code)
	{
		if (ins.opcode == OP_DCL_OUTPUT || ins.opcode == OP_DCL_TEMPS)
			return insertAt ? insertAt : ins.at;
		if (ins.opcode != OP_DCL_INPUT_PS)
			continue;
		Operand operand;
		if (!ReadOperand(words, ins.operandsAt, ins.at + ins.length, operand))
			continue;
		if (operand.index0 == input)
		{
			words[operand.at] |= mask << 4;
			return 0;
		}
		if (operand.index0 > input && !insertAt)
			insertAt = ins.at;
	}
	return insertAt;
}

void WidenConstants(std::vector<uint32_t>& words, const std::vector<Instruction>& code, uint32_t buffer, uint32_t vectors)
{
	for (const Instruction& ins : code)
	{
		if (ins.opcode != OP_DCL_CONSTANT_BUFFER)
			continue;
		Operand operand;
		if (ReadOperand(words, ins.operandsAt, ins.at + ins.length, operand) && operand.dimension == 2 && operand.index0 == buffer && operand.index1 < vectors)
			words[operand.indexAt + 1] = vectors;
	}
}

std::vector<uint8_t> BuildContainer(const std::vector<Chunk>& chunks, const std::vector<uint32_t>& program)
{
	struct Part
	{
		uint32_t tag;
		const uint8_t* data;
		uint32_t size;
	};
	std::vector<Part> parts;
	for (const Chunk& chunk : chunks)
	{
		if (chunk.tag == TAG_ISGN || chunk.tag == TAG_OSGN)
			parts.push_back({chunk.tag, chunk.data, chunk.size});
	}
	parts.push_back({TAG_SHDR, reinterpret_cast<const uint8_t*>(program.data()), uint32_t(program.size() * 4)});

	uint32_t total = 32 + uint32_t(parts.size()) * 4;
	for (const Part& part : parts)
		total += 8 + part.size;
	std::vector<uint8_t> out(total);
	memcpy(out.data(), "DXBC", 4);
	const uint32_t one = 1, count = uint32_t(parts.size());
	memcpy(out.data() + 20, &one, 4);
	memcpy(out.data() + 24, &total, 4);
	memcpy(out.data() + 28, &count, 4);
	uint32_t offset = 32 + count * 4;
	for (uint32_t i = 0; i < count; ++i)
	{
		memcpy(out.data() + 32 + i * 4, &offset, 4);
		memcpy(out.data() + offset, &parts[i].tag, 4);
		memcpy(out.data() + offset + 4, &parts[i].size, 4);
		memcpy(out.data() + offset + 8, parts[i].data, parts[i].size);
		offset += 8 + parts[i].size;
	}
	RetailHash(out.data() + 20, out.size() - 20, out.data() + 4);
	return out;
}
}

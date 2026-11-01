#pragma once
#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

// The parts of T6's Material > MaterialTechniqueSet > MaterialTechnique > MaterialPass > MaterialPixelShader chain used here,
// and reading and writing game memory that may go away at any moment: zones unload behind our back.
namespace Pom::Game
{
	// The sorted material table.
	constexpr uintptr_t MATERIAL_TABLE = 0x35A9380;
	constexpr size_t MATERIAL_COUNT = 4096;
	constexpr size_t MATERIAL_TECHSET = 0x5C;
	constexpr size_t TECHSET_TECHNIQUES = 8;
	constexpr size_t TECHSET_SIZE = TECHSET_TECHNIQUES + 36 * 4;
	constexpr size_t TECHNIQUE_PASS_COUNT = 6;
	constexpr size_t TECHNIQUE_PASSES = 8;
	constexpr size_t PASS_SIZE = 24;
	constexpr size_t PASS_PIXEL_SHADER = 8;
	constexpr size_t PIXEL_SHADER_OBJECT = 4;
	constexpr size_t PIXEL_SHADER_CODE = 8;
	constexpr size_t PIXEL_SHADER_BYTES = 12;
	constexpr size_t PIXEL_SHADER_SIZE = 16;

	// "lit" .. "lit omni shadow dlight glight", the 11 lit techniques and their twins.
	constexpr int LIT_FIRST = 0x04;
	constexpr int LIT_LAST = 0x19;
	constexpr unsigned MAX_PASSES = 8;
	constexpr uint32_t MAX_SHADER_BYTES = 256 * 1024;

	inline bool Read(uintptr_t address, void* out, size_t size)
	{
		SIZE_T copied = 0;
		return address && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), out, size, &copied) && copied == size;
	}

	template <typename T>
	bool ReadValue(uintptr_t address, T& out)
	{
		return Read(address, &out, sizeof(T));
	}

	inline bool WritePointer(uintptr_t address, uintptr_t value)
	{
		__try
		{
			InterlockedExchange(reinterpret_cast<volatile LONG*>(address), LONG(value));
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	inline bool ExchangePointer(uintptr_t address, void* value, void** old)
	{
		__try
		{
			*old = InterlockedExchangePointer(reinterpret_cast<void* volatile*>(address), value);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// A shader's code, as the game loaded it. `units` says how the descriptor counts its size, in bytes or dwords: it depends
	// on how the shader was loaded, and the container knows the truth.
	inline bool ReadShaderCode(uint32_t code, uint32_t size, void* out, size_t capacity, uint32_t& bytes, uint32_t& units)
	{
		uint8_t header[32];
		if (!Read(code, header, sizeof(header)) || memcmp(header, "DXBC", 4) != 0)
			return false;
		memcpy(&bytes, header + 24, 4);
		if (bytes < 32 || bytes > MAX_SHADER_BYTES || bytes > capacity || (size != bytes && size != bytes / 4))
			return false;
		units = size == bytes ? 1 : 4;
		return Read(code, out, bytes);
	}
}

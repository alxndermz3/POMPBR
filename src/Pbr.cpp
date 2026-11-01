#include "Pbr.h"

#include <d3d11.h>

#include <cstring>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Brdf.h"
#include "GameMemory.h"
#include "Log.h"
#include "Reflect.h"

namespace Pom::Pbr
{
namespace
{
	using namespace Game;

	constexpr unsigned MAX_FAILURES_LOGGED = 16;

	// A patched shader. The same code turns up in many records, across zones and materials, so they share one of these.
	struct Made
	{
		ID3D11PixelShader* shader = nullptr;  // Null if the patch had nothing to do.
		std::shared_ptr<std::vector<uint8_t>> code;
	};

	struct Entry
	{
		ID3D11PixelShader* native = nullptr;   // Off, it only says the record is still the same one. On, it's the game's reference.
		ID3D11PixelShader* patched = nullptr;  // One reference is ours, and one the game's while on.
		std::shared_ptr<std::vector<uint8_t>> code;
		uint32_t nativeCode = 0, nativeSize = 0;
		uint32_t patchedSize = 0;
		bool on = false;
	};

	std::unordered_map<uintptr_t, Entry> g_Entries;  // By descriptor.
	bool g_On = false;
	unsigned g_Failures = 0;
	std::vector<uint8_t> g_Scratch;

	uint64_t HashOf(const uint8_t* bytes, size_t size)
	{
		uint64_t hash = 14695981039346656037ull ^ size;
		for (size_t i = 0; i < size; ++i)
		{
			hash ^= bytes[i];
			hash *= 1099511628211ull;
		}
		return hash;
	}

	bool ObjectIs(uintptr_t descriptor, ID3D11PixelShader* object)
	{
		uint32_t current = 0;
		return ReadValue(descriptor + PIXEL_SHADER_OBJECT, current) && current == uint32_t(reinterpret_cast<uintptr_t>(object));
	}

	void Put(uintptr_t descriptor, ID3D11PixelShader* object, uint32_t code, uint32_t size)
	{
		WritePointer(descriptor + PIXEL_SHADER_OBJECT, reinterpret_cast<uintptr_t>(object));
		WritePointer(descriptor + PIXEL_SHADER_CODE, code);
		WritePointer(descriptor + PIXEL_SHADER_BYTES, size);
	}

	void TurnOn(uintptr_t descriptor, Entry& entry)
	{
		entry.patched->AddRef();  // The reference the game owns through its record.
		void* old = nullptr;
		if (!ExchangePointer(descriptor + PIXEL_SHADER_OBJECT, entry.patched, &old))
		{
			entry.patched->Release();
			return;
		}
		entry.native = static_cast<ID3D11PixelShader*>(old);
		WritePointer(descriptor + PIXEL_SHADER_CODE, reinterpret_cast<uintptr_t>(entry.code->data()));
		WritePointer(descriptor + PIXEL_SHADER_BYTES, entry.patchedSize);
		entry.on = true;
	}

	void TurnOff(uintptr_t descriptor, Entry& entry)
	{
		Put(descriptor, entry.native, entry.nativeCode, entry.nativeSize);
		entry.patched->Release();
		entry.on = false;
	}

	// Its record is gone or replaced: nothing of it is ours to put back, except that a vid_restart leaves our code there.
	void LetGo(uintptr_t descriptor, Entry& entry)
	{
		uint32_t code = 0;
		if (entry.on && ReadValue(descriptor + PIXEL_SHADER_CODE, code) && code == uint32_t(reinterpret_cast<uintptr_t>(entry.code->data())))
		{
			WritePointer(descriptor + PIXEL_SHADER_CODE, entry.nativeCode);
			WritePointer(descriptor + PIXEL_SHADER_BYTES, entry.nativeSize);
		}
		if (entry.on)
			entry.native->Release();
		entry.patched->Release();
	}

	bool StillThere(uintptr_t descriptor, const Entry& entry)
	{
		return ObjectIs(descriptor, entry.on ? entry.patched : entry.native);
	}

	void Forget()
	{
		for (auto it = g_Entries.begin(); it != g_Entries.end();)
		{
			if (StillThere(it->first, it->second))
			{
				++it;
				continue;
			}
			LetGo(it->first, it->second);
			it = g_Entries.erase(it);
		}
	}

	void Take(uintptr_t descriptor, std::unordered_map<uint64_t, Made>& made)
	{
		uint32_t object = 0, code = 0, size = 0;
		if (!ReadValue(descriptor + PIXEL_SHADER_OBJECT, object) || !ReadValue(descriptor + PIXEL_SHADER_CODE, code) || !ReadValue(descriptor + PIXEL_SHADER_BYTES, size) || !object || !code)
			return;
		uint32_t bytes = 0, units = 1;
		if (!ReadShaderCode(code, size, g_Scratch.data(), g_Scratch.size(), bytes, units))
			return;

		const uint64_t hash = HashOf(g_Scratch.data(), bytes);
		auto found = made.find(hash);
		if (found == made.end())
		{
			Made fresh;
			const Brdf::Result result = Brdf::Patch(g_Scratch.data(), bytes);
			if (!result.ok && g_Failures++ < MAX_FAILURES_LOGGED)
				Log::Write("PBR couldn't patch a shader: %s", result.error.c_str());
			if (result.changed)
			{
				ID3D11Device* device = nullptr;
				reinterpret_cast<ID3D11PixelShader*>(uintptr_t(object))->GetDevice(&device);
				const HRESULT hr = device ? device->CreatePixelShader(result.shader.data(), result.shader.size(), nullptr, &fresh.shader) : E_FAIL;
				if (device)
					device->Release();
				if (SUCCEEDED(hr))
					fresh.code = std::make_shared<std::vector<uint8_t>>(result.shader);
				else
				{
					fresh.shader = nullptr;
					if (g_Failures++ < MAX_FAILURES_LOGGED)
						Log::Write("the driver refused a PBR shader: 0x%08X", unsigned(hr));
				}
			}
			found = made.emplace(hash, std::move(fresh)).first;
		}
		if (!found->second.shader)
			return;

		Entry entry;
		entry.patched = found->second.shader;
		entry.patched->AddRef();
		entry.code = found->second.code;
		entry.nativeCode = code;
		entry.nativeSize = size;
		entry.patchedSize = uint32_t(entry.code->size()) / units;
		TurnOn(descriptor, entry);
		g_Entries.emplace(descriptor, std::move(entry));
	}
}

void Refresh()
{
	const ULONGLONG start = GetTickCount64();
	g_Scratch.resize(MAX_SHADER_BYTES);
	Forget();
	for (auto& entry : g_Entries)
	{
		if (!entry.second.on)
			TurnOn(entry.first, entry.second);
	}

	static uintptr_t table[MATERIAL_COUNT];
	std::unordered_map<uint64_t, Made> made;
	const size_t before = g_Entries.size();
	if (Read(MATERIAL_TABLE, table, sizeof(table)))
	{
		std::unordered_set<uintptr_t> techsets, techniques, descriptors;
		for (const uintptr_t material : table)
		{
			uintptr_t techset = 0;
			if (!material || !ReadValue(material + MATERIAL_TECHSET, techset) || !techset || !techsets.insert(techset).second)
				continue;
			for (int t = LIT_FIRST; t <= LIT_LAST; ++t)
			{
				uintptr_t technique = 0;
				uint16_t passes = 0;
				if (!ReadValue(techset + TECHSET_TECHNIQUES + t * 4, technique) || !technique || !techniques.insert(technique).second ||
					!ReadValue(technique + TECHNIQUE_PASS_COUNT, passes) || !passes || passes > MAX_PASSES)
					continue;
				for (unsigned p = 0; p < passes; ++p)
				{
					uintptr_t descriptor = 0;
					if (ReadValue(technique + TECHNIQUE_PASSES + p * PASS_SIZE + PASS_PIXEL_SHADER, descriptor) && descriptor && descriptors.insert(descriptor).second &&
						!g_Entries.count(descriptor))
						Take(descriptor, made);
				}
			}
		}
	}

	unsigned created = 0;
	for (auto& entry : made)
	{
		if (entry.second.shader)
		{
			++created;
			entry.second.shader->Release();  // The entries have theirs.
		}
	}
	g_Scratch = {};
	g_On = true;

	auto shaders = std::make_unique<Reflect::Shaders>();
	for (const auto& entry : g_Entries)
		shaders->insert(entry.second.patched);
	Reflect::SetShaders(shaders.release());
	Log::Write("PBR on: %u shaders in use, %u new, %u made in %u ms", unsigned(g_Entries.size()), unsigned(g_Entries.size() - before), created, unsigned(GetTickCount64() - start));
}

void Off()
{
	Reflect::SetShaders(nullptr);
	Forget();
	for (auto& entry : g_Entries)
	{
		if (entry.second.on)
			TurnOff(entry.first, entry.second);
	}
	g_On = false;
	Log::Write("PBR off");
}

bool IsOn()
{
	return g_On;
}

bool Lost()
{
	if (g_Entries.empty())
		return false;
	const auto first = g_Entries.begin();
	return first->second.on && !ObjectIs(first->first, first->second.patched);
}
}

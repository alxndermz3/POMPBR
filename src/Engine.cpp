#include "Engine.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <utility>

#include "Bake.h"
#include "Dxbc.h"
#include "GameMemory.h"
#include "Log.h"
#include "MinHook.h"
#include "Pbr.h"
#include "PomFile.h"
#include "Reflect.h"
#include "Settings.h"

// A material on the list gets a copy of its technique set, in which every lit technique uses a patched pixel shader, and
// the material's pointer is moved to that copy. Nothing runs per draw: the game batches, sorts and binds as it always
// does. The height travels in the blue channel of a copy of the normal map, swapped in behind the GfxImage, which is why
// no shader needs an extra texture bound, and why the blended variants of a surface (cobble + dirt, cobble + a decal
// layer) follow along: the game generates those as separate materials, all pointing at the same normal map.
// Depth prepass, shadow maps and everything else keep using the native techniques.
namespace Pom::Engine
{
namespace
{
	using namespace Game;

	// Hooked. Only its call matters, for running once per view with the map's assets in place: the arguments aren't read.
	constexpr uintptr_t FN_RENDER_VIEW = 0x72E880;

	// Only compared, to be sure of the exe.
	constexpr uintptr_t FN_SORT_DRAW_SURFS = 0x752770;
	constexpr uintptr_t FN_GAME_MODE_IS = 0x6F7570;

	struct Signature
	{
		uintptr_t address;
		uint8_t bytes[12];
	};

	const Signature SIGNATURES[] = {
		{FN_RENDER_VIEW, {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0xA1, 0x90, 0xBE, 0x5E, 0x03, 0x8B}},
		{FN_SORT_DRAW_SURFS, {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x81, 0xEC, 0x0C, 0x01, 0x00, 0x00}},
		{FN_GAME_MODE_IS, {0x8B, 0x4C, 0x24, 0x04, 0xB8, 0x01, 0x00, 0x00, 0x00, 0xD3, 0xE0, 0x85}},
	};

	// The parts of Material and MaterialTextureDef used here. The rest of the chain is in GameMemory.h.
	constexpr size_t MATERIAL_NAME = 0;
	constexpr size_t MATERIAL_TEXTURE_COUNT = 84;
	constexpr size_t MATERIAL_TEXTURES = 96;
	constexpr size_t TEXTURE_DEF_SIZE = 16;
	constexpr size_t TEXTURE_DEF_NAME_START = 4;
	constexpr size_t TEXTURE_DEF_NAME_END = 5;
	constexpr size_t TEXTURE_DEF_IMAGE = 12;
	constexpr size_t IMAGE_TEXTURE = 0;  // GfxImage::texture, the view the game binds.
	constexpr size_t IMAGE_NAME = 72;

	constexpr unsigned MAX_TEXTURES = 32;
	constexpr ULONGLONG CHECK_INTERVAL_MS = 250;
	constexpr ULONGLONG RETIRE_MS = 3000;
	constexpr ULONGLONG RUNNING_MS = 1000;

	struct Memory
	{
		std::vector<std::unique_ptr<uint8_t[]>> blocks;
		std::vector<IUnknown*> objects;

		Memory() = default;
		Memory(Memory&&) = default;
		Memory& operator=(Memory&& other)
		{
			Clear();
			blocks = std::move(other.blocks);
			objects = std::move(other.objects);
			other.blocks.clear();
			other.objects.clear();
			return *this;
		}
		~Memory() { Clear(); }

		void Clear()
		{
			for (IUnknown* object : objects)
				object->Release();
			objects.clear();
			blocks.clear();
		}
	};

	using Key = uintptr_t;

	struct Texture
	{
		std::string name;
		PomHeader header{};
		uintptr_t image = 0;                       // The GfxImage, while swapped.
		ID3D11ShaderResourceView* view = nullptr;  // One reference is ours, and one is the game's while swapped.
		void* native = nullptr;                    // What the image held, with the reference the game had on it.
		Dxbc::Literals literals{};                 // What its shaders were made with.
		std::map<Key, uintptr_t> shaders, techniques, techsets;  // Copies made so far, by original.
		Memory memory;
	};

	struct Swapped
	{
		uintptr_t material;
		uintptr_t native;
		uintptr_t clone;
		Texture* texture;
	};

	struct Retired
	{
		ULONGLONG at;
		Memory memory;
	};

	void* g_RenderView = nullptr;

	Settings g_Settings;
	std::map<std::string, Texture> g_Textures;
	std::vector<Swapped> g_Swaps;
	std::deque<Retired> g_Retired;  // Replaced while a frame may still be drawing with it.

	std::mutex g_JobsLock;
	std::vector<std::function<void()>> g_Jobs;
	std::atomic<ULONGLONG> g_LastView{0};

	uint64_t g_MaterialHash = 0;
	bool g_Applied = false;
	bool g_ToggleWasDown = false;
	bool g_PbrToggleWasDown = false;
	ULONGLONG g_LastCheck = 0;
	unsigned g_ShadersMade = 0, g_ShaderFailures = 0, g_ImagesReclaimed = 0;

	// One native shader: vid_restart makes new shader objects for the same materials.
	uintptr_t g_SentinelDescriptor = 0;
	uint32_t g_SentinelObject = 0;

	std::string ReadString(uintptr_t address)
	{
		char text[128] = {};
		// Up to the end of the page first: the next one may not be there.
		const size_t first = (std::min)(sizeof(text) - 1, size_t(4096 - (address & 4095)));
		if (!Read(address, text, first))
			return {};
		if (first < sizeof(text) - 1 && strnlen(text, first) == first)
			Read(address + first, text + first, sizeof(text) - 1 - first);
		return text;
	}

	std::string Lower(std::string text)
	{
		std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(tolower(c)); });
		return text;
	}

	uint64_t HashMaterials()
	{
		static uintptr_t table[MATERIAL_COUNT];
		if (!Read(MATERIAL_TABLE, table, sizeof(table)))
			return 0;
		uint64_t hash = 14695981039346656037ull;
		for (const uintptr_t pointer : table)
		{
			hash ^= pointer;
			hash *= 1099511628211ull;
		}
		return hash;
	}

	// The images a material uses for its layer 0 color and normal maps. Texture defs keep the first and last letter of
	// their name, which tells colorMap and normalMap from each other and from colorMap1, normalMap1 and so on.
	void ImagesOf(uintptr_t material, uintptr_t& color, uintptr_t& normal)
	{
		color = normal = 0;
		uint8_t count = 0;
		uintptr_t table = 0;
		if (!ReadValue(material + MATERIAL_TEXTURE_COUNT, count) || !ReadValue(material + MATERIAL_TEXTURES, table) || !table)
			return;
		uint8_t defs[MAX_TEXTURES * TEXTURE_DEF_SIZE];
		const unsigned wanted = (std::min)(unsigned(count), MAX_TEXTURES);
		if (!wanted || !Read(table, defs, wanted * TEXTURE_DEF_SIZE))
			return;
		for (unsigned i = 0; i < wanted; ++i)
		{
			const uint8_t* def = defs + i * TEXTURE_DEF_SIZE;
			uintptr_t* slot = nullptr;
			if (def[TEXTURE_DEF_NAME_START] == 'n' && def[TEXTURE_DEF_NAME_END] == 'p')
				slot = &normal;
			else if (def[TEXTURE_DEF_NAME_START] == 'c' && def[TEXTURE_DEF_NAME_END] == 'p')
				slot = &color;
			if (slot)
				memcpy(slot, def + TEXTURE_DEF_IMAGE, sizeof(*slot));
		}
	}

	std::string ImageName(uintptr_t image)
	{
		uintptr_t text = 0;
		return ReadValue(image + IMAGE_NAME, text) && text ? ReadString(text) : std::string();
	}

	std::string MaterialName(uintptr_t material)
	{
		uintptr_t text = 0;
		return ReadValue(material + MATERIAL_NAME, text) && text ? ReadString(text) : std::string("?");
	}

	std::set<std::string> ListFiles()
	{
		std::set<std::string> names;
		WIN32_FIND_DATAA found{};
		const HANDLE find = FindFirstFileA((SettingsFile::TextureFolder() + "\\*.pom").c_str(), &found);
		if (find == INVALID_HANDLE_VALUE)
			return names;
		do
		{
			std::string name = found.cFileName;
			names.insert(Lower(name.substr(0, name.size() - 4)));
		} while (FindNextFileA(find, &found));
		FindClose(find);
		return names;
	}

	ID3D11Device* DeviceOfTechset(uintptr_t techset)
	{
		for (int t = LIT_FIRST; t <= LIT_LAST; ++t)
		{
			uintptr_t technique = 0, descriptor = 0;
			uint32_t object = 0;
			if (!ReadValue(techset + TECHSET_TECHNIQUES + t * 4, technique) || !technique ||
				!ReadValue(technique + TECHNIQUE_PASSES + PASS_PIXEL_SHADER, descriptor) || !descriptor ||
				!ReadValue(descriptor + PIXEL_SHADER_OBJECT, object) || !object)
				continue;
			if (!g_SentinelDescriptor)
			{
				g_SentinelDescriptor = descriptor;
				g_SentinelObject = object;
			}
			ID3D11Device* device = nullptr;
			reinterpret_cast<ID3D11PixelShader*>(uintptr_t(object))->GetDevice(&device);
			return device;
		}
		return nullptr;
	}

	bool LoadTexture(ID3D11Device* device, Texture& texture)
	{
		PomFile file;
		std::string error;
		const std::string path = SettingsFile::TextureFolder() + "\\" + texture.name + ".pom";
		if (!ReadPomFile(path, file, error))
		{
			Log::Write("%s: %s", path.c_str(), error.c_str());
			return false;
		}
		texture.header = file.header;

		// Files from before crevice shading: once, and kept, so it's not worked out at every load.
		if (!Bake::HasShading(file))
		{
			Bake::AddShading(file);
			if (!WritePomFile(path, file, error))
				Log::Write("%s: %s", path.c_str(), error.c_str());
		}

		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = file.header.width;
		desc.Height = file.header.height;
		desc.MipLevels = file.header.mips;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_IMMUTABLE;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		std::vector<D3D11_SUBRESOURCE_DATA> levels(file.header.mips);
		const uint8_t* pixels = file.pixels.data();
		uint32_t width = file.header.width, height = file.header.height;
		for (uint32_t i = 0; i < file.header.mips; ++i)
		{
			levels[i] = {pixels, width * 4, 0};
			pixels += size_t(width) * height * 4;
			width = (std::max)(width / 2, 1u);
			height = (std::max)(height / 2, 1u);
		}
		ID3D11Texture2D* resource = nullptr;
		HRESULT hr = device->CreateTexture2D(&desc, levels.data(), &resource);
		if (SUCCEEDED(hr))
		{
			hr = device->CreateShaderResourceView(resource, nullptr, &texture.view);
			resource->Release();
		}
		if (FAILED(hr))
		{
			Log::Write("%s: the texture wasn't created: 0x%08X", texture.name.c_str(), unsigned(hr));
			texture.view = nullptr;
			return false;
		}
		return true;
	}

	Dxbc::Literals LiteralsFor(const Texture& texture)
	{
		Dxbc::Literals literals;
		literals.depthUv = texture.header.depthUv * SettingsFile::DepthScale(texture.name);
		literals.mostSteps = QUALITY_LEVELS[g_Settings.quality].mostSteps;
		literals.fewestSteps = QUALITY_LEVELS[g_Settings.quality].fewestSteps;
		literals.steepest = g_Settings.steepest;
		literals.textureWidth = float(texture.header.width);
		literals.fadeEnd = g_Settings.fadeEnd;
		literals.fadeScale = 1.0f / (std::max)(g_Settings.fadeEnd - g_Settings.fadeStart, 1.0f);
		literals.traceSteps = QUALITY_LEVELS[g_Settings.quality].traceSteps;
		literals.crevice = g_Settings.crevice;
		literals.reliefShadows = g_Settings.reliefShadows;
		return literals;
	}

	uint8_t* Allocate(Texture& texture, size_t size)
	{
		texture.memory.blocks.push_back(std::make_unique<uint8_t[]>(size));
		return texture.memory.blocks.back().get();
	}

	uintptr_t CopyOfShader(uintptr_t descriptor, Texture& texture, ID3D11Device* device)
	{
		const auto known = texture.shaders.find(descriptor);
		if (known != texture.shaders.end())
			return known->second;

		uint32_t code = 0, size = 0, bytes = 0, units = 1;
		if (!ReadValue(descriptor + PIXEL_SHADER_CODE, code) || !ReadValue(descriptor + PIXEL_SHADER_BYTES, size))
			return 0;
		std::vector<uint8_t> original(MAX_SHADER_BYTES);
		if (!ReadShaderCode(code, size, original.data(), original.size(), bytes, units))
			return 0;

		const Dxbc::Result result = Dxbc::Patch(original.data(), bytes, texture.literals);
		if (!result.ok)
		{
			if (g_ShaderFailures++ < 16)
				Log::Write("couldn't patch a shader: %s", result.error.c_str());
			return 0;
		}
		ID3D11PixelShader* shader = nullptr;
		const HRESULT hr = device->CreatePixelShader(result.shader.data(), result.shader.size(), nullptr, &shader);
		if (FAILED(hr))
		{
			if (g_ShaderFailures++ < 16)
				Log::Write("the driver refused a patched shader: 0x%08X", unsigned(hr));
			return 0;
		}
		texture.memory.objects.push_back(shader);
		++g_ShadersMade;

		// MoreLights reads the code of whatever a material uses, so the copy keeps the shader it describes.
		uint8_t* stored = Allocate(texture, result.shader.size());
		memcpy(stored, result.shader.data(), result.shader.size());
		uint8_t* copy = Allocate(texture, PIXEL_SHADER_SIZE);
		if (!Read(descriptor, copy, PIXEL_SHADER_SIZE))
			return 0;
		const uint32_t object = uint32_t(reinterpret_cast<uintptr_t>(shader));
		const uint32_t storedAt = uint32_t(reinterpret_cast<uintptr_t>(stored));
		const uint32_t storedSize = uint32_t(result.shader.size());
		memcpy(copy + PIXEL_SHADER_OBJECT, &object, 4);
		memcpy(copy + PIXEL_SHADER_CODE, &storedAt, 4);
		memcpy(copy + PIXEL_SHADER_BYTES, &storedSize, 4);
		return texture.shaders[descriptor] = reinterpret_cast<uintptr_t>(copy);
	}

	uintptr_t CopyOfTechnique(uintptr_t technique, Texture& texture, ID3D11Device* device)
	{
		const auto known = texture.techniques.find(technique);
		if (known != texture.techniques.end())
			return known->second;

		uint16_t passes = 0;
		if (!ReadValue(technique + TECHNIQUE_PASS_COUNT, passes) || !passes || passes > MAX_PASSES)
			return 0;
		const size_t size = TECHNIQUE_PASSES + passes * PASS_SIZE;
		uint8_t* copy = Allocate(texture, size);
		if (!Read(technique, copy, size))
			return 0;
		for (unsigned p = 0; p < passes; ++p)
		{
			const size_t at = TECHNIQUE_PASSES + p * PASS_SIZE + PASS_PIXEL_SHADER;
			uintptr_t descriptor = 0;
			memcpy(&descriptor, copy + at, 4);
			if (!descriptor)
				continue;
			const uintptr_t shader = CopyOfShader(descriptor, texture, device);
			if (!shader)
				return 0;
			memcpy(copy + at, &shader, 4);
		}
		return texture.techniques[technique] = reinterpret_cast<uintptr_t>(copy);
	}

	// All lit techniques or none: a surface that loses its relief whenever a light moves over it looks like a bug.
	uintptr_t CopyOfTechset(uintptr_t techset, Texture& texture, ID3D11Device* device)
	{
		const auto known = texture.techsets.find(techset);
		if (known != texture.techsets.end())
			return known->second;

		uint8_t* copy = Allocate(texture, TECHSET_SIZE);
		if (!Read(techset, copy, TECHSET_SIZE))
			return 0;
		for (int t = LIT_FIRST; t <= LIT_LAST; ++t)
		{
			const size_t at = TECHSET_TECHNIQUES + t * 4;
			uintptr_t technique = 0;
			memcpy(&technique, copy + at, 4);
			if (!technique)
				continue;
			const uintptr_t patched = CopyOfTechnique(technique, texture, device);
			if (!patched)
				return 0;
			memcpy(copy + at, &patched, 4);
		}
		return texture.techsets[techset] = reinterpret_cast<uintptr_t>(copy);
	}

	// How many references besides ours exist: the game's, while it still holds the view.
	ULONG ReferencesOn(ID3D11ShaderResourceView* view)
	{
		view->AddRef();
		return view->Release();
	}

	void SwapImage(Texture& texture, uintptr_t image)
	{
		void* old = nullptr;
		texture.view->AddRef();  // The reference the game owns through GfxImage::texture.
		if (!ExchangePointer(image + IMAGE_TEXTURE, texture.view, &old))
		{
			texture.view->Release();
			return;
		}
		texture.image = image;
		texture.native = old;
	}

	// Puts an image back while the game's reference is still on our view.
	void RestoreImage(Texture& texture)
	{
		if (!texture.image)
			return;
		uintptr_t current = 0;
		if (ReadValue(texture.image + IMAGE_TEXTURE, current) && current == reinterpret_cast<uintptr_t>(texture.view) &&
			ReferencesOn(texture.view) >= 2)
		{
			void* ours = nullptr;
			if (ExchangePointer(texture.image + IMAGE_TEXTURE, texture.native, &ours))
			{
				texture.view->Release();
				texture.native = nullptr;
			}
		}
		// Otherwise the game let go of ours, freeing the image or replacing its texture, and left the native one to us.
		if (texture.native)
			static_cast<IUnknown*>(texture.native)->Release();
		texture.native = nullptr;
		texture.image = 0;
	}

	bool IsSwapped(uintptr_t material)
	{
		return std::any_of(g_Swaps.begin(), g_Swaps.end(), [&](const Swapped& swap) { return swap.material == material; });
	}

	// Puts every material whose normal map is in `files` and isn't swapped yet on POM.
	void Scan(const std::set<std::string>& files, unsigned& refused)
	{
		refused = 0;
		static uintptr_t table[MATERIAL_COUNT];
		if (!Read(MATERIAL_TABLE, table, sizeof(table)))
			return;
		for (const uintptr_t material : table)
		{
			if (!material || IsSwapped(material))
				continue;
			uintptr_t color = 0, image = 0;
			ImagesOf(material, color, image);
			if (!image)
				continue;
			const std::string name = Lower(ImageName(image));
			if (name.empty() || !files.count(name))
				continue;
			uintptr_t techset = 0;
			if (!ReadValue(material + MATERIAL_TECHSET, techset) || !techset)
				continue;
			ID3D11Device* device = DeviceOfTechset(techset);
			if (!device)
				continue;

			Texture& texture = g_Textures[name];
			const bool fresh = !texture.view;
			texture.name = name;
			uintptr_t clone = 0;
			if (!fresh || LoadTexture(device, texture))
			{
				if (fresh || texture.shaders.empty())
					texture.literals = LiteralsFor(texture);
				clone = CopyOfTechset(techset, texture, device);
			}
			device->Release();
			if (!clone)
			{
				++refused;
				Log::Write("left alone: %s", MaterialName(material).c_str());
				if (!texture.view)
					g_Textures.erase(name);
				continue;
			}
			if (!texture.image)
				SwapImage(texture, image);
			if (WritePointer(material + MATERIAL_TECHSET, clone))
				g_Swaps.push_back({material, techset, clone, &texture});
			Log::Write("  %s (%s)", MaterialName(material).c_str(), name.c_str());
		}
	}

	void ApplyAll()
	{
		g_Applied = true;
		const std::set<std::string> files = ListFiles();
		if (files.empty())
		{
			Log::Write("nothing to do: no .pom files in %s", SettingsFile::TextureFolder().c_str());
			return;
		}
		const ULONGLONG start = GetTickCount64();
		unsigned refused = 0;
		Scan(files, refused);
		Log::Write("on: %u materials over %u textures, %u shaders made in %u ms", unsigned(g_Swaps.size()), unsigned(g_Textures.size()), g_ShadersMade,
			unsigned(GetTickCount64() - start));
		g_ShadersMade = 0;
	}

	void RestoreMaterial(const Swapped& swap)
	{
		// A material still alive after the table changed (a zone loaded next to the map) is pointing at memory that's
		// about to go. One that's gone holds something else here by now, or nothing anyone reads.
		uintptr_t current = 0;
		if (ReadValue(swap.material + MATERIAL_TECHSET, current) && current == swap.clone)
			WritePointer(swap.material + MATERIAL_TECHSET, swap.native);
	}

	void RestoreMaterialsOf(const Texture* texture)
	{
		auto kept = g_Swaps.begin();
		for (auto it = g_Swaps.begin(); it != g_Swaps.end(); ++it)
		{
			if (!texture || it->texture == texture)
				RestoreMaterial(*it);
			else
				*kept++ = *it;
		}
		g_Swaps.erase(kept, g_Swaps.end());
	}

	void Undo()
	{
		RestoreMaterialsOf(nullptr);
		for (auto& entry : g_Textures)
			RestoreImage(entry.second);
		g_Applied = false;
	}

	// A frame being drawn may still use what's going, so it's kept for a few seconds.
	void Retire(Texture& texture)
	{
		texture.shaders.clear();
		texture.techniques.clear();
		texture.techsets.clear();
		g_Retired.push_back({GetTickCount64(), std::move(texture.memory)});
	}

	void RetireWhole(Texture& texture)
	{
		Retire(texture);
		if (texture.view)
			g_Retired.back().memory.objects.push_back(texture.view);
		texture.view = nullptr;
	}

	void PurgeRetired()
	{
		const ULONGLONG now = GetTickCount64();
		while (!g_Retired.empty() && now - g_Retired.front().at > RETIRE_MS)
			g_Retired.pop_front();
	}

	// The material table changed. That's usually a new map, but a zone loaded next to the map changes it too, so this
	// can run with materials alive and a frame in flight.
	void Forget()
	{
		Undo();
		for (auto& entry : g_Textures)
			RetireWhole(entry.second);
		g_Textures.clear();
		g_SentinelDescriptor = 0;
	}

	// The stream thread can replace an image's texture when it loads better mips, releasing what it finds there.
	void GuardImages()
	{
		for (auto& entry : g_Textures)
		{
			Texture& texture = entry.second;
			uintptr_t current = 0;
			if (!texture.image || !ReadValue(texture.image + IMAGE_TEXTURE, current) || !current ||
				current == reinterpret_cast<uintptr_t>(texture.view))
				continue;
			void* old = nullptr;
			texture.view->AddRef();
			if (!ExchangePointer(texture.image + IMAGE_TEXTURE, texture.view, &old))
			{
				texture.view->Release();
				continue;
			}
			if (texture.native)
				static_cast<IUnknown*>(texture.native)->Release();
			texture.native = old;
			if (g_ImagesReclaimed++ < 16)
				Log::Write("%s: the game replaced its texture, taken back", texture.name.c_str());
		}
	}

	bool MapChanged(uint64_t hash)
	{
		if (hash && hash != g_MaterialHash)
			return true;
		// The same map again: same pointers, fresh contents.
		for (const Swapped& swap : g_Swaps)
		{
			uintptr_t current = 0;
			if (!ReadValue(swap.material + MATERIAL_TECHSET, current) || current != swap.clone)
				return true;
		}
		return false;
	}

	bool DeviceChanged()
	{
		uint32_t object = 0;
		return g_SentinelDescriptor && ReadValue(g_SentinelDescriptor + PIXEL_SHADER_OBJECT, object) && object != g_SentinelObject;
	}

	// Settings came in. Only textures whose shaders would differ are redone: a new depth for one doesn't touch the rest.
	void ApplySettings()
	{
		for (auto& entry : g_Textures)
		{
			Texture& texture = entry.second;
			const Dxbc::Literals wanted = LiteralsFor(texture);
			if (memcmp(&wanted, &texture.literals, sizeof(wanted)) == 0)
				continue;
			RestoreMaterialsOf(&texture);
			Retire(texture);
			texture.literals = wanted;
		}
		ApplyAll();
	}

	Brdf::Constants ConstantsOf(const PbrSettings& pbr)
	{
		Brdf::Constants constants;
		constants.roughnessOffset = pbr.roughness - 1;
		constants.highlightsOffset = pbr.highlights - 1;
		constants.reflectionsOffset = pbr.reflections - 1;
		constants.screenAmount = pbr.screenReflections;
		constants.screenSteps = float(pbr.screenSteps);
		constants.screenReach = pbr.screenReach;
		return constants;
	}

	// POM's copies are made from what a shader's code is at the time, so they're remade around any change to it.
	void SyncPbr(bool again)
	{
		const bool wanted = g_Settings.pbr.enabled;
		if (wanted == Pbr::IsOn() && !(wanted && again))
			return;
		const bool pomOn = g_Applied;
		if (pomOn)
		{
			Undo();
			for (auto& entry : g_Textures)
				Retire(entry.second);
		}
		if (wanted)
			Pbr::Refresh();
		else
			Pbr::Off();
		g_SentinelDescriptor = 0;
	}

	void RunJobs()
	{
		std::vector<std::function<void()>> jobs;
		{
			std::lock_guard<std::mutex> lock(g_JobsLock);
			jobs.swap(g_Jobs);
		}
		for (auto& job : jobs)
			job();
	}

	void __cdecl OnView()
	{
		g_LastView.store(GetTickCount64(), std::memory_order_relaxed);
		const bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_F8) & 0x8000);
		const bool toggled = down && !g_ToggleWasDown;
		g_ToggleWasDown = down;
		if (toggled)
		{
			g_Settings.enabled = !g_Settings.enabled;
			SettingsFile::SaveEnabled(g_Settings.enabled);
			Log::Write("CTRL+F8: %s", g_Settings.enabled ? "on" : "off");
			MessageBeep(g_Settings.enabled ? MB_OK : MB_ICONASTERISK);
		}
		const bool pbrDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_F7) & 0x8000);
		const bool pbrToggled = pbrDown && !g_PbrToggleWasDown;
		g_PbrToggleWasDown = pbrDown;
		if (pbrToggled)
		{
			g_Settings.pbr.enabled = !g_Settings.pbr.enabled;
			SettingsFile::SavePbr(g_Settings.pbr);
			Log::Write("CTRL+F7: PBR %s", g_Settings.pbr.enabled ? "on" : "off");
			MessageBeep(g_Settings.pbr.enabled ? MB_OK : MB_ICONASTERISK);
		}

		const ULONGLONG now = GetTickCount64();
		if (toggled || pbrToggled || now - g_LastCheck >= CHECK_INTERVAL_MS)
		{
			g_LastCheck = now;
			const bool settingsChanged = SettingsFile::Reload(g_Settings);
			if (settingsChanged)
				Reflect::SetConstants(ConstantsOf(g_Settings.pbr));
			const uint64_t hash = HashMaterials();
			const bool lost = MapChanged(hash) || DeviceChanged() || Pbr::Lost();
			if (lost)
			{
				Forget();
				g_MaterialHash = hash;
			}
			else if (settingsChanged && g_Applied && g_Settings.enabled)
			{
				ApplySettings();
			}
			SyncPbr(lost);

			if (g_Settings.enabled && !g_Applied)
				ApplyAll();
			else if (!g_Settings.enabled && g_Applied)
				Undo();
			if (g_Applied)
				GuardImages();
			PurgeRetired();
		}
		RunJobs();
	}

	__declspec(naked) void RenderView_Thunk()
	{
		__asm
		{
			pushfd
			pushad
			mov ebp, esp
			sub esp, 528
			and esp, 0xFFFFFFF0
			fxsave [esp]
			call OnView
			fxrstor [esp]
			mov esp, ebp
			popad
			popfd
			jmp g_RenderView
		}
	}
}

bool Install()
{
	SettingsFile::EnsureExists();
	SettingsFile::Reload(g_Settings);
	Reflect::SetConstants(ConstantsOf(g_Settings.pbr));

	if (reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)) != 0x400000)
	{
		Log::Write("unexpected image base: not installed");
		return false;
	}
	for (const Signature& s : SIGNATURES)
	{
		if (memcmp(reinterpret_cast<const void*>(s.address), s.bytes, sizeof(s.bytes)) != 0)
		{
			Log::Write("this isn't t6mpv43 as expected: bytes at %08X differ, not installed", unsigned(s.address));
			return false;
		}
	}
	void* target = reinterpret_cast<void*>(FN_RENDER_VIEW);
	MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&RenderView_Thunk), &g_RenderView);
	if (status == MH_OK)
		status = MH_EnableHook(target);
	if (status != MH_OK)
	{
		Log::Write("hooking R_RenderView failed: %s", MH_StatusToString(status));
		return false;
	}
	return true;
}

void Post(std::function<void()> job)
{
	std::lock_guard<std::mutex> lock(g_JobsLock);
	g_Jobs.push_back(std::move(job));
}

bool MapIsRunning()
{
	const ULONGLONG last = g_LastView.load(std::memory_order_relaxed);
	return last && GetTickCount64() - last < RUNNING_MS;
}

int ToolKey()
{
	return g_Settings.toolKey;
}

std::shared_ptr<const Picker::Table> BuildTable()
{
	auto result = std::make_shared<Picker::Table>();
	static uintptr_t table[MATERIAL_COUNT];
	if (!Read(MATERIAL_TABLE, table, sizeof(table)))
		return result;
	for (const uintptr_t material : table)
	{
		if (!material)
			continue;
		uintptr_t color = 0, normal = 0;
		ImagesOf(material, color, normal);
		void* colorView = nullptr;
		void* normalView = nullptr;
		if (!color || !ReadValue(color + IMAGE_TEXTURE, colorView) || !colorView)
			continue;
		if (normal)
			ReadValue(normal + IMAGE_TEXTURE, normalView);
		result->surfaces[Picker::Table::Key(colorView, normalView)] = material;
	}
	return result;
}

Surface Describe(uintptr_t material)
{
	Surface surface;
	surface.material = MaterialName(material);
	uintptr_t color = 0, normal = 0;
	ImagesOf(material, color, normal);
	if (color)
		surface.colorImage = ImageName(color);
	if (normal)
		surface.normalImage = ImageName(normal);

	if (!normal || surface.normalImage.empty())
		surface.refused = "This material has no normal map, and POM needs one of its own.";
	else if (surface.normalImage[0] == '$')
		surface.refused = "This material uses the game's shared flat normal map, so POM would hit everything else on it.";
	else if (surface.normalImage.find_first_of("\\/:*?\"<>|") != std::string::npos)
		surface.refused = "This texture's name can't be a file name.";

	static uintptr_t table[MATERIAL_COUNT];
	if (normal && Read(MATERIAL_TABLE, table, sizeof(table)))
	{
		for (const uintptr_t other : table)
		{
			if (!other)
				continue;
			uintptr_t otherColor = 0, otherNormal = 0;
			ImagesOf(other, otherColor, otherNormal);
			surface.sharing += otherNormal == normal;
		}
	}

	const auto viewOf = [](uintptr_t image)
	{
		void* raw = nullptr;
		if (image)
			ReadValue(image + IMAGE_TEXTURE, raw);
		ID3D11ShaderResourceView* view = static_cast<ID3D11ShaderResourceView*>(raw);
		if (view)
			view->AddRef();
		return view;
	};
	surface.colorView = viewOf(color);
	surface.normalView = viewOf(normal);

	const std::string name = Lower(surface.normalImage);
	surface.converted = g_Textures.count(name) != 0;
	surface.depthScale = SettingsFile::DepthScale(name);
	return surface;
}

std::vector<Converted> List()
{
	std::vector<Converted> list;
	for (const auto& entry : g_Textures)
	{
		Converted item;
		item.image = entry.first;
		item.depthScale = SettingsFile::DepthScale(entry.first);
		item.materials = unsigned(std::count_if(g_Swaps.begin(), g_Swaps.end(), [&](const Swapped& swap) { return swap.texture == &entry.second; }));
		list.push_back(item);
	}
	return list;
}

Applied ApplyTexture(const std::string& image)
{
	const std::string name = Lower(image);
	const auto existing = g_Textures.find(name);
	if (existing != g_Textures.end())
	{
		RestoreMaterialsOf(&existing->second);
		RestoreImage(existing->second);
		RetireWhole(existing->second);
		g_Textures.erase(existing);
	}
	if (!g_Settings.enabled)
	{
		g_Settings.enabled = true;
		SettingsFile::SaveEnabled(true);
		ApplyAll();
	}

	Applied result;
	const ULONGLONG start = GetTickCount64();
	g_ShadersMade = 0;
	Scan({name}, result.refused);
	const auto applied = g_Textures.find(name);
	if (applied != g_Textures.end())
		result.materials = unsigned(std::count_if(g_Swaps.begin(), g_Swaps.end(), [&](const Swapped& swap) { return swap.texture == &applied->second; }));
	Log::Write("%s: on %u materials, %u left alone, %u shaders made in %u ms", name.c_str(), result.materials, result.refused, g_ShadersMade,
		unsigned(GetTickCount64() - start));
	g_ShadersMade = 0;
	return result;
}

void RemoveTexture(const std::string& image)
{
	const std::string name = Lower(image);
	const auto existing = g_Textures.find(name);
	if (existing != g_Textures.end())
	{
		RestoreMaterialsOf(&existing->second);
		RestoreImage(existing->second);
		RetireWhole(existing->second);
		g_Textures.erase(existing);
	}
	DeleteFileA((SettingsFile::TextureFolder() + "\\" + name + ".pom").c_str());
	SettingsFile::ForgetDepth(name);
	Log::Write("%s: removed", name.c_str());
}

void SetDepthScale(const std::string& image, float scale)
{
	const std::string name = Lower(image);
	SettingsFile::SetDepthScale(name, scale);
	const auto existing = g_Textures.find(name);
	if (existing == g_Textures.end())
		return;
	RestoreMaterialsOf(&existing->second);
	Retire(existing->second);
	existing->second.literals = LiteralsFor(existing->second);
	unsigned refused = 0;
	Scan({name}, refused);
}

Look ReadLook()
{
	return {g_Settings.quality, g_Settings.fadeStart, g_Settings.fadeEnd, g_Settings.crevice, g_Settings.reliefShadows};
}

void SetLook(const Look& look)
{
	g_Settings.quality = look.quality;
	g_Settings.fadeStart = look.fadeStart;
	g_Settings.fadeEnd = look.fadeEnd;
	g_Settings.crevice = look.crevice;
	g_Settings.reliefShadows = look.reliefShadows;
	SettingsFile::SaveLook(look.quality, look.fadeStart, look.fadeEnd, look.crevice, look.reliefShadows);
	Log::Write("look: quality %s, fade %.0f..%.0f, crevices %.2f, relief shadows %.2f", QUALITY_LEVELS[look.quality].name, look.fadeStart, look.fadeEnd,
		look.crevice, look.reliefShadows);
	if (g_Applied && g_Settings.enabled)
		ApplySettings();
}

PbrSettings ReadPbr()
{
	return g_Settings.pbr;
}

void PreviewPbr(const PbrSettings& pbr)
{
	Reflect::SetConstants(ConstantsOf(pbr));
}

void SetPbr(const PbrSettings& pbr)
{
	g_Settings.pbr = pbr;
	SettingsFile::SavePbr(pbr);
	Reflect::SetConstants(ConstantsOf(pbr));
	SyncPbr(false);
	if (g_Settings.enabled && !g_Applied)
		ApplyAll();
}
}

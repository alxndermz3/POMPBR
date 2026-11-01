#include "Tool.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

#include "Bake.h"
#include "Decode.h"
#include "Engine.h"
#include "Log.h"
#include "Panel.h"
#include "PomFile.h"
#include "Settings.h"
#include "imgui.h"

namespace Pom::Tool
{
namespace
{
	constexpr unsigned SMALLEST_NORMAL_MAP = 64;  // Below this it's a stand-in, not a surface's own texture.
	constexpr ULONGLONG DEPTH_DRAG_MS = 150;

	template <typename T>
	void SafeRelease(T*& object)
	{
		if (object)
		{
			object->Release();
			object = nullptr;
		}
	}

	struct Selection
	{
		uintptr_t material = 0;
		Engine::Surface surface;
		UINT width = 0, height = 0;
	};

	// Shared between the threads, under g_Lock.
	std::mutex g_Lock;
	Selection g_Selection;
	bool g_Selected = false;
	bool g_Busy = false;
	bool g_ViewsChanged = false;
	int g_Preset = 0;
	std::string g_Status;
	bool g_StatusIsBad = false;
	std::vector<Panel::Item> g_List;
	std::shared_ptr<const Picker::Table> g_NewTable;
	bool g_PickWithNewTable = false;
	Engine::Look g_Look;
	PbrSettings g_Pbr;

	// Render thread only.
	Picker::Core g_Core;
	std::string g_KeyName = "F7";
	float g_PickU = 0, g_PickV = 0;
	float g_DepthDragging = -1;
	ULONGLONG g_LastDepthSent = 0;

	void SetStatus(const std::string& text, bool bad)
	{
		g_Status = text;
		g_StatusIsBad = bad;
	}

	void ClearSelection()
	{
		SafeRelease(g_Selection.surface.colorView);
		SafeRelease(g_Selection.surface.normalView);
		g_Selection = Selection();
		g_Selected = false;
		g_ViewsChanged = true;
	}

	// How big a view's texture is. The panel shows it, and a tiny one is a generic stand-in.
	void SizeOf(ID3D11ShaderResourceView* view, UINT& width, UINT& height)
	{
		width = height = 0;
		if (!view)
			return;
		ID3D11Resource* resource = nullptr;
		view->GetResource(&resource);
		ID3D11Texture2D* texture = nullptr;
		if (resource)
			resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
		SafeRelease(resource);
		if (!texture)
			return;
		D3D11_TEXTURE2D_DESC desc{};
		texture->GetDesc(&desc);
		texture->Release();
		width = desc.Width;
		height = desc.Height;
	}

	void RefreshList()
	{
		std::vector<Panel::Item> list;
		for (const Engine::Converted& item : Engine::List())
			list.push_back({item.image, item.materials});
		std::lock_guard<std::mutex> lock(g_Lock);
		g_List = std::move(list);
	}

	// On the game's thread: reads the material again, after anything that changed what's bound to it.
	void Select(uintptr_t material)
	{
		Engine::Surface surface = Engine::Describe(material);
		UINT width = 0, height = 0;
		SizeOf(surface.normalView, width, height);
		if (surface.refused.empty() && width && width < SMALLEST_NORMAL_MAP && !surface.converted)
			surface.refused = "This normal map is only a small generic one, shared and flat. POM needs a surface's own.";

		std::lock_guard<std::mutex> lock(g_Lock);
		SafeRelease(g_Selection.surface.colorView);
		SafeRelease(g_Selection.surface.normalView);
		g_Selection.material = material;
		g_Selection.surface = std::move(surface);
		g_Selection.width = width;
		g_Selection.height = height;
		g_Selected = true;
		g_ViewsChanged = true;
	}

	void Finish(uintptr_t material, const std::string& status, bool bad)
	{
		if (material)
			Select(material);
		RefreshList();
		std::lock_guard<std::mutex> lock(g_Lock);
		g_Busy = false;
		SetStatus(status, bad);
	}

	// On a thread of its own: half a second for a 512 texture, and the game keeps running.
	void BakeAndApply(std::vector<Bake::Pixels> normal, Bake::Pixels color, std::string image, int preset, uintptr_t material)
	{
		const Bake::Recipe recipe = Bake::RecipeFor(Bake::Preset(preset));
		PomFile file;
		std::string error;
		const ULONGLONG start = GetTickCount64();
		std::string name = image;
		std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(tolower(c)); });
		CreateDirectoryA(SettingsFile::TextureFolder().c_str(), nullptr);
		bool ok = false;
		try
		{
			ok = Bake::Build(normal, color, recipe, file, error) && WritePomFile(SettingsFile::TextureFolder() + "\\" + name + ".pom", file, error);
		}
		catch (const std::bad_alloc&)
		{
			error = "the game is out of memory for a texture this size";
		}
		if (!ok)
		{
			Log::Write("%s: bake failed: %s", name.c_str(), error.c_str());
			Engine::Post([error] { Finish(0, "Couldn't convert it: " + error, true); });
			return;
		}
		Log::Write("%s: baked %ux%u in %u ms", name.c_str(), file.header.width, file.header.height, unsigned(GetTickCount64() - start));
		Engine::Post([material, name]
		{
			const Engine::Applied applied = Engine::ApplyTexture(name);
			if (!applied.materials)
			{
				Engine::RemoveTexture(name);
				Finish(material, "This surface's shaders can't take POM.", true);
			}
			else if (applied.refused)
				Finish(material, "On " + std::to_string(applied.materials) + " materials. " + std::to_string(applied.refused) + " use shaders that can't take POM.", true);
			else
				Finish(material, "On " + std::to_string(applied.materials) + (applied.materials == 1 ? " material." : " materials."), false);
		});
	}

	// The picked pixels of a bake come from the GPU, so this has to run on the render thread. The bake itself doesn't.
	void Convert(ID3D11DeviceContext* context)
	{
		ID3D11ShaderResourceView* normalView = nullptr;
		ID3D11ShaderResourceView* colorView = nullptr;
		std::string image;
		uintptr_t material = 0;
		int preset = 0;
		{
			std::lock_guard<std::mutex> lock(g_Lock);
			if (g_Busy || !g_Selected || !g_Selection.surface.refused.empty())
				return;
			normalView = g_Selection.surface.normalView;
			colorView = g_Selection.surface.colorView;
			if (normalView)
				normalView->AddRef();
			if (colorView)
				colorView->AddRef();
			image = g_Selection.surface.normalImage;
			material = g_Selection.material;
			preset = g_Preset;
			g_Busy = true;
			SetStatus("", false);
		}

		std::string error;
		std::vector<Bake::Pixels> normal, color;
		bool ok = normalView && Decode::Read(context, normalView, normal, error);
		if (ok && colorView)
		{
			std::string colorError;
			if (!Decode::Read(context, colorView, color, colorError))
				Log::Write("%s: no color map to read seams from: %s", image.c_str(), colorError.c_str());
		}
		SafeRelease(normalView);
		SafeRelease(colorView);
		if (!ok)
		{
			std::lock_guard<std::mutex> lock(g_Lock);
			g_Busy = false;
			SetStatus("Couldn't read the normal map: " + error, true);
			return;
		}
		if (color.empty())
		{
			Bake::Pixels flat;
			flat.width = flat.height = 1;
			flat.rgba = {128, 128, 128, 255};
			color.push_back(std::move(flat));
		}
		std::thread(BakeAndApply, std::move(normal), std::move(color[0]), std::move(image), preset, material).detach();
	}

	void Remove(const std::string& image, uintptr_t material)
	{
		{
			std::lock_guard<std::mutex> lock(g_Lock);
			if (g_Busy)
				return;
			g_Busy = true;
			SetStatus("", false);
		}
		Engine::Post([image, material]
		{
			Engine::RemoveTexture(image);
			Finish(material, "Removed.", false);
		});
	}

	void SendDepth(const std::string& image, float scale, uintptr_t material)
	{
		Engine::Post([image, scale, material]
		{
			Engine::SetDepthScale(image, scale);
			Select(material);
		});
	}
}

bool Init(ID3D11Device* device, std::string& error)
{
	return g_Core.Init(device, error);
}

void SetKeyName(const char* name)
{
	g_KeyName = name;
}

Picker::Core& Picking()
{
	return g_Core;
}

void Open()
{
	{
		std::lock_guard<std::mutex> lock(g_Lock);
		SetStatus("", false);
	}
	Engine::Post([]
	{
		RefreshList();
		const Engine::Look look = Engine::ReadLook();
		const PbrSettings pbr = Engine::ReadPbr();
		std::lock_guard<std::mutex> lock(g_Lock);
		g_Look = look;
		g_Pbr = pbr;
	});
}

void Close()
{
	std::lock_guard<std::mutex> lock(g_Lock);
	ClearSelection();
}

void EndFrame(ID3D11DeviceContext* context, UINT screenWidth, UINT screenHeight)
{
	g_Core.SetScreenSize(screenWidth, screenHeight);
	{
		std::lock_guard<std::mutex> lock(g_Lock);
		if (g_NewTable)
		{
			g_Core.SetTable(std::move(g_NewTable));
			g_NewTable = nullptr;
			if (g_PickWithNewTable)
				g_Core.RequestPick(g_PickU, g_PickV);
			g_PickWithNewTable = false;
		}
		if (g_ViewsChanged)
		{
			g_Core.SetHighlight(g_Selected ? g_Selection.surface.normalView : nullptr);
			g_ViewsChanged = false;
		}
	}

	Picker::Hit hit;
	if (!g_Core.EndFrame(context, hit))
		return;
	const Picker::Report& report = g_Core.LastReport();
	Log::Write("pick at %.3f, %.3f: %u draws, %u solid, %u known, %u reached the pixel, %u surfaces in the table -> %s", g_PickU, g_PickV, report.draws,
		report.solid, report.known, report.reached, report.tableSize, hit.material ? "found" : "nothing");
	SafeRelease(hit.color);
	SafeRelease(hit.normal);
	if (!hit.material)
	{
		std::lock_guard<std::mutex> lock(g_Lock);
		ClearSelection();
		SetStatus("Nothing that can take POM there.", true);
		return;
	}
	const uintptr_t material = hit.material;
	{
		std::lock_guard<std::mutex> lock(g_Lock);
		g_Preset = 0;
		SetStatus("", false);
	}
	Engine::Post([material] { Select(material); });
}

bool Draw(ID3D11DeviceContext* context)
{
	Panel::Model model;
	uintptr_t material = 0;
	{
		std::lock_guard<std::mutex> lock(g_Lock);
		model.keyName = g_KeyName;
		model.selected = g_Selected;
		model.material = g_Selection.surface.material;
		model.normalImage = g_Selection.surface.normalImage;
		model.refused = g_Selection.surface.refused;
		model.sharing = g_Selection.surface.sharing;
		model.width = g_Selection.width;
		model.height = g_Selection.height;
		model.converted = g_Selection.surface.converted;
		model.depthScale = g_DepthDragging >= 0 ? std::copysign(g_DepthDragging, g_Selection.surface.depthScale) : g_Selection.surface.depthScale;
		model.preset = g_Preset;
		model.busy = g_Busy;
		model.status = g_Status;
		model.statusIsBad = g_StatusIsBad;
		model.list = g_List;
		model.quality = g_Look.quality;
		model.fadeStart = g_Look.fadeStart;
		model.fadeEnd = g_Look.fadeEnd;
		model.crevice = g_Look.crevice;
		model.reliefShadows = g_Look.reliefShadows;
		model.pbr = g_Pbr.enabled;
		model.highlights = g_Pbr.highlights;
		model.roughness = g_Pbr.roughness;
		model.reflections = g_Pbr.reflections;
		model.screenReflections = g_Pbr.screenReflections;
		model.screenSteps = g_Pbr.screenSteps;
		model.screenReach = g_Pbr.screenReach;
		material = g_Selection.material;
	}

	Panel::Pressed pressed;
	Panel::Draw(model, pressed);

	if (pressed.presetChanged)
	{
		{
			std::lock_guard<std::mutex> lock(g_Lock);
			g_Preset = pressed.preset;
		}
		if (model.converted)
			Convert(context);
	}
	if (pressed.convert)
		Convert(context);
	if (pressed.remove)
		Remove(model.normalImage, material);
	if (!pressed.removeImage.empty())
		Remove(pressed.removeImage, model.selected ? material : 0);

	if (pressed.depthChanged)
	{
		g_DepthDragging = pressed.depthScale;
		const ULONGLONG now = GetTickCount64();
		if (now - g_LastDepthSent >= DEPTH_DRAG_MS)
		{
			g_LastDepthSent = now;
			SendDepth(model.normalImage, std::copysign(pressed.depthScale, model.depthScale), material);
		}
	}
	if (pressed.depthReleased)
	{
		g_DepthDragging = -1;
		SendDepth(model.normalImage, std::copysign(pressed.depthScale, model.depthScale), material);
	}
	if (pressed.invertToggled)
		SendDepth(model.normalImage, -model.depthScale, material);

	if (pressed.lookChanged)
	{
		std::lock_guard<std::mutex> lock(g_Lock);
		g_Look = {pressed.quality, pressed.fadeStart, pressed.fadeEnd, pressed.crevice, pressed.reliefShadows};
	}
	if (pressed.lookReleased)
	{
		const Engine::Look look{pressed.quality, pressed.fadeStart, pressed.fadeEnd, pressed.crevice, pressed.reliefShadows};
		Engine::Post([look] { Engine::SetLook(look); });
	}

	if (pressed.pbrChanged || pressed.pbrReleased || pressed.pbrToggled)
	{
		PbrSettings pbr;
		{
			std::lock_guard<std::mutex> lock(g_Lock);
			g_Pbr.enabled = pressed.pbr;
			g_Pbr.highlights = pressed.highlights;
			g_Pbr.roughness = pressed.roughness;
			g_Pbr.reflections = pressed.reflections;
			g_Pbr.screenReflections = pressed.screenReflections;
			g_Pbr.screenSteps = pressed.screenSteps;
			g_Pbr.screenReach = pressed.screenReach;
			pbr = g_Pbr;
		}
		Engine::PreviewPbr(pbr);
		if (pressed.pbrReleased || pressed.pbrToggled)
			Engine::Post([pbr] { Engine::SetPbr(pbr); });
	}

	// A click on the world picks what's under it. The table is made fresh: streaming swaps textures all the time.
	const ImGuiIO& io = ImGui::GetIO();
	if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.WantCaptureMouse && !ImGui::IsAnyItemActive())
	{
		g_PickU = io.MousePos.x / io.DisplaySize.x;
		g_PickV = io.MousePos.y / io.DisplaySize.y;
		Engine::Post([]
		{
			std::shared_ptr<const Picker::Table> table = Engine::BuildTable();
			std::lock_guard<std::mutex> lock(g_Lock);
			g_NewTable = std::move(table);
			g_PickWithNewTable = true;
		});
	}
	return pressed.close;
}
}

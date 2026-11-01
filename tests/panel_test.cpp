// Draws the tool's window without the game, for looking at: panel_test.exe <state> <out.ppm>
//   state: empty, stone, converted, inverted, refused, busy
// A click can be scripted too, which checks the buttons report what they should: panel_test.exe clicks

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "../src/Bake.h"
#include "../src/Panel.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"

#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;
using namespace Pom;

namespace
{
	constexpr UINT WIDTH = 1280, HEIGHT = 720;

	Panel::Model ModelFor(const std::string& state)
	{
		Panel::Model model;
		model.keyName = "F7";
		model.quality = 3;
		model.fadeStart = 240;
		model.fadeEnd = 1100;
		model.crevice = 0.6f;
		model.reliefShadows = 1;
		model.list = {{"rock_cobble_stone_pak_n", 12}, {"t6_wood_panel_n", 9}, {"tree_oak_trunk_nml", 1}, {"ma_tile_wood_01_nml", 2}};
		if (state == "empty")
			return model;
		model.selected = true;
		model.material = "wpc/rock_cobble_stone_pak";
		model.normalImage = "rock_cobble_stone_pak_n";
		model.width = model.height = 256;
		model.sharing = 12;
		if (state == "converted")
		{
			model.converted = true;
			model.depthScale = 2;
			model.status = "On 12 materials.";
		}
		else if (state == "inverted")
		{
			model.converted = true;
			model.depthScale = -2;
		}
		else if (state == "refused")
		{
			model.material = "wpc/ny_nyse_wood_floor_01";
			model.normalImage = "global_normal_flat_16x16";
			model.width = model.height = 16;
			model.sharing = 11;
			model.refused = "This normal map is only a small generic one, shared and flat. POM needs a surface's own.";
		}
		else if (state == "busy")
			model.busy = true;
		return model;
	}

	struct Lab
	{
		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> context;
		ComPtr<ID3D11Texture2D> target, readback;
		ComPtr<ID3D11RenderTargetView> rtv;
	};

	void Frame(Lab& lab, const Panel::Model& model, Panel::Pressed& pressed, float seconds = 0.016f)
	{
		ImGui::GetIO().DeltaTime = seconds;
		ImGui_ImplDX11_NewFrame();
		ImGui::NewFrame();
		Panel::Draw(model, pressed);
		ImGui::Render();
		const float clear[4] = {0.25f, 0.27f, 0.25f, 1};
		lab.context->ClearRenderTargetView(lab.rtv.Get(), clear);
		lab.context->OMSetRenderTargets(1, lab.rtv.GetAddressOf(), nullptr);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
	}

	void Save(Lab& lab, const char* path)
	{
		lab.context->CopyResource(lab.readback.Get(), lab.target.Get());
		D3D11_MAPPED_SUBRESOURCE map{};
		lab.context->Map(lab.readback.Get(), 0, D3D11_MAP_READ, 0, &map);
		std::ofstream out(path, std::ios::binary);
		out << "P6\n" << WIDTH << " " << HEIGHT << "\n255\n";
		for (UINT y = 0; y < HEIGHT; ++y)
		{
			const uint8_t* row = static_cast<const uint8_t*>(map.pData) + size_t(y) * map.RowPitch;
			for (UINT x = 0; x < WIDTH; ++x)
				out.write(reinterpret_cast<const char*>(row + size_t(x) * 4), 3);
		}
		lab.context->Unmap(lab.readback.Get(), 0);
	}

	// Moves the mouse there, presses, releases, and returns what the panel reported over those frames.
	Panel::Pressed Click(Lab& lab, const Panel::Model& model, float x, float y)
	{
		ImGuiIO& io = ImGui::GetIO();
		Panel::Pressed total, pressed;
		io.AddMousePosEvent(x, y);
		Frame(lab, model, pressed);
		Frame(lab, model, pressed);
		io.AddMouseButtonEvent(0, true);
		Frame(lab, model, pressed);
		io.AddMouseButtonEvent(0, false);
		for (int i = 0; i < 2; ++i)
		{
			Panel::Pressed frame;
			Frame(lab, model, frame);
			total.convert |= frame.convert;
			total.remove |= frame.remove;
			total.close |= frame.close;
			total.invertToggled |= frame.invertToggled;
		}
		return total;
	}

	// Where an item ended up, found by hovering down the panel's column: what matters is that some spot on it does the job.
	bool FindButton(Lab& lab, const Panel::Model& model, bool Panel::Pressed::* field, float leftFrom, float leftTo)
	{
		for (float y = 30; y < 480; y += 8)
		{
			for (float x = leftFrom; x <= leftTo; x += 60)
			{
				if (Click(lab, model, x, y).*field)
					return true;
			}
		}
		return false;
	}
}

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		puts("panel_test <empty|stone|converted|inverted|refused|busy> <out.ppm> | clicks");
		return 2;
	}
	Lab lab;
	const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
	if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &lab.device, nullptr, &lab.context)))
		return 1;
	D3D11_TEXTURE2D_DESC d{};
	d.Width = WIDTH;
	d.Height = HEIGHT;
	d.MipLevels = d.ArraySize = 1;
	d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	d.SampleDesc.Count = 1;
	d.BindFlags = D3D11_BIND_RENDER_TARGET;
	lab.device->CreateTexture2D(&d, nullptr, &lab.target);
	lab.device->CreateRenderTargetView(lab.target.Get(), nullptr, &lab.rtv);
	d.BindFlags = 0;
	d.Usage = D3D11_USAGE_STAGING;
	d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	lab.device->CreateTexture2D(&d, nullptr, &lab.readback);
	const D3D11_VIEWPORT viewport{0, 0, float(WIDTH), float(HEIGHT), 0, 1};
	lab.context->RSSetViewports(1, &viewport);

	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.DisplaySize = ImVec2(float(WIDTH), float(HEIGHT));
	io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f);
	Panel::Style();
	ImGui_ImplDX11_Init(lab.device.Get(), lab.context.Get());

	const std::string state = argv[1];
	if (state == "clicks")
	{
		int failed = 0;
		const float left = WIDTH - 390 - 24 + 40, right = WIDTH - 24 - 40;
		const auto check = [&](const char* what, const Panel::Model& model, bool Panel::Pressed::* field)
		{
			const bool found = FindButton(lab, model, field, left, right);
			failed += !found;
			printf("%-30s %s\n", what, found ? "reports" : "WRONG: never reported");
		};
		check("Convert to POM", ModelFor("stone"), &Panel::Pressed::convert);
		check("Bake again", ModelFor("converted"), &Panel::Pressed::convert);
		check("Remove POM", ModelFor("converted"), &Panel::Pressed::remove);
		check("Invert", ModelFor("converted"), &Panel::Pressed::invertToggled);
		check("Close", ModelFor("empty"), &Panel::Pressed::close);

		const Panel::Model busy = ModelFor("busy");
		const bool quiet = !FindButton(lab, busy, &Panel::Pressed::convert, left, right);
		failed += !quiet;
		printf("%-30s %s\n", "busy: Convert does nothing", quiet ? "ok" : "WRONG");
		const Panel::Model refused = ModelFor("refused");
		const bool none = !FindButton(lab, refused, &Panel::Pressed::convert, left, right);
		failed += !none;
		printf("%-30s %s\n", "refused: nothing to convert", none ? "ok" : "WRONG");
		return failed ? 1 : 0;
	}

	const Panel::Model model = ModelFor(state);
	io.AddMousePosEvent(200, 300);
	for (int i = 0; i < 6; ++i)
	{
		Panel::Pressed pressed;
		Frame(lab, model, pressed);
	}
	Save(lab, argv[2]);
	return 0;
}

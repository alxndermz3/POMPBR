#pragma once
#include <d3d11.h>

#include <string>

#include "Picker.h"

// The picker tool: what happens between clicking a surface and it having POM. Runs on the render thread, from Present;
// material work goes to the game's thread through Engine::Post, and baking to a thread of its own.
namespace Pom::Tool
{
	bool Init(ID3D11Device* device, std::string& error);
	void SetKeyName(const char* name);

	void Open();
	void Close();

	// Once a frame while it's open or just closed, whether or not the window is drawn.
	void EndFrame(ID3D11DeviceContext* context, UINT screenWidth, UINT screenHeight);

	// Between ImGui::NewFrame and Render. True if it wants to close.
	bool Draw(ID3D11DeviceContext* context);

	Picker::Core& Picking();
}

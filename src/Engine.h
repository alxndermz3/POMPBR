#pragma once
#include <d3d11.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Picker.h"
#include "Settings.h"

namespace Pom::Engine
{
	// Checks the exe and hooks in. Nothing changes in the game until a map with a matching .pom file is on screen.
	bool Install();

	// Runs a job on the game's thread at the next view, between frames' worth of material work. Everything below that
	// touches materials has to run in one.
	void Post(std::function<void()> job);

	// A world is being rendered right now.
	bool MapIsRunning();
	int ToolKey();

	struct Surface
	{
		std::string material;
		std::string normalImage, colorImage;
		std::string refused;    // Why it can't take POM; empty if it can.
		unsigned sharing = 0;   // Materials on the same normal map, which all change together.
		bool converted = false;
		float depthScale = 1;
		ID3D11ShaderResourceView* colorView = nullptr;   // What's bound right now, with a reference each for the caller.
		ID3D11ShaderResourceView* normalView = nullptr;
	};

	struct Converted
	{
		std::string image;
		unsigned materials = 0;
		float depthScale = 1;
	};

	struct Applied
	{
		unsigned materials = 0;
		unsigned refused = 0;  // Materials on this normal map whose shaders can't take it.
	};

	std::shared_ptr<const Picker::Table> BuildTable();
	Surface Describe(uintptr_t material);
	std::vector<Converted> List();

	// The .pom for this image was just written: puts it on every material using that normal map.
	Applied ApplyTexture(const std::string& image);
	void RemoveTexture(const std::string& image);
	void SetDepthScale(const std::string& image, float scale);  // Below 0 is inverted.

	// For every texture on POM, so a change here remakes their shaders.
	struct Look
	{
		int quality = 0;
		float fadeStart = 0, fadeEnd = 0;
		float crevice = 0, reliefShadows = 0;
	};

	Look ReadLook();
	void SetLook(const Look& look);

	// The sliders go straight to the shaders while they're dragged, and SetPbr keeps the result.
	PbrSettings ReadPbr();
	void PreviewPbr(const PbrSettings& pbr);  // From any thread.
	void SetPbr(const PbrSettings& pbr);
}

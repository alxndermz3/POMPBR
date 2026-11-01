#pragma once
#include <d3d11.h>

#include <cstddef>
#include <unordered_set>

#include "Brdf.h"

// What the patched shaders read beyond the game's own bindings: the constants at b12, and for screen reflections last
// frame's color at t16 and depth at t17. Slots above t15 are ones the game never touches, so one bind per scene is enough
// and nothing runs per draw, and an unbound slot reads as nothing to reflect, so a shader falls back on its probe.
//
// The scene is found by its shaders: the first patched one to be set after anything else opens it, with the render targets
// bound at that moment. Whatever changes those targets closes it, and that's when color and depth get copied. Post effects
// and the HUD come after that, so they stay out of reflections. Passes smaller than half the screen are extra cameras.
//
// All of it is called from the render thread, through the hooks in Overlay.cpp, except SetShaders and SetConstants.
namespace Pom::Reflect
{
	using Shaders = std::unordered_set<ID3D11PixelShader*>;

	// Which shaders mark the scene. Null while PBR is off, and then nothing here runs. The last few sets are kept, as a
	// frame may still be reading one.
	void SetShaders(const Shaders* shaders);
	void SetConstants(const Brdf::Constants& constants);

	void BeforeShader(ID3D11DeviceContext* context, ID3D11PixelShader* shader);
	void BeforeTargets(ID3D11DeviceContext* context, UINT count, ID3D11RenderTargetView* const* targets, ID3D11DepthStencilView* depth);
	void EndFrame(ID3D11DeviceContext* context, UINT screenWidth, UINT screenHeight);
	void Release();  // The device is going, or the swap chain's being resized.
}

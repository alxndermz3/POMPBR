#pragma once
#include <d3d11.h>

#include <string>
#include <vector>

#include "Bake.h"

// Reading textures back out of the game.
namespace Pom::Decode
{
	// One mip of any format the game uses for material textures, as RGBA8. `pitch` is the distance between rows of texels
	// (rows of blocks, for BC formats). False if the format isn't one of them.
	bool Level(DXGI_FORMAT format, uint32_t width, uint32_t height, const uint8_t* data, uint32_t pitch, Bake::Pixels& out);

	// Every mip of a texture from the GPU, largest first, so it gets what's really on screen, a retexture included. Mips
	// the streamer hasn't filled yet are left out. Immediate context only: the render thread's.
	bool Read(ID3D11DeviceContext* context, ID3D11ShaderResourceView* view, std::vector<Bake::Pixels>& out, std::string& error);
}

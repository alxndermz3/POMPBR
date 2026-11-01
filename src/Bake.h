#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "PomFile.h"

// Makes a .pom from a normal map and color map, inside the game. No D3D here: only pixels in, pixels out.
//
// The world shaders do  N = normalize(n + x * tangent + y * binormal),  with x and y the two channels of the normal map
// mapped to +-2.0157: the map holds surface slopes. Integrating those gives the height it was made from, so the parallax
// matches the shading. The height goes in the blue channel of a copy of the normal map, which the BC5 original doesn't
// have, and the patched shader reads it there. tools/bake_pom.py is the same thing in numpy, and tests compare the two.
namespace Pom::Bake
{
	struct Pixels
	{
		uint32_t width = 0, height = 0;
		std::vector<uint8_t> rgba;  // Top to bottom, four bytes a pixel.
	};

	// What kind of surface it is. Lengths are texels of a 256 texture, so a recipe fits any size.
	struct Recipe
	{
		float depth = 0.017f;   // How deep the texture goes, in UV units.
		float gap = 0.75f;      // How much of the depth the dark seams of the color map drop on top of the shape.
		float blur = 0.7f;      // Seam softness.
		float flatten = 24;     // Anything broader than this in the normal map is drift from integrating, not shape.
		float radius = 6;       // How wide a dark line can be and still count as a seam.
		float soften = 0.35f;   // Blur of the finished height. Sharp height on a small map smears color down steep faces.
		float tops = 92;        // Percentile of the height that counts as the surface...
		float bottoms = 3;      // ...and as the deepest point.
		bool plain = false;     // Level surface, normal map too flat to integrate: height comes from the seams alone.
		float grain = 0;        // On a plain surface, how much of the depth the dark grain of the color map drops.
	};

	enum class Preset
	{
		Auto,    // The same as Detail, which suits the most surfaces.
		Stone,   // Cobble, brick, blocks: raised shapes with gaps.
		Boards,  // Floors and panels: level, with thin seams.
		Bark,    // Rough all over, no real surface plane.
		Detail,  // Metal, machined parts, atlases: flat with details carved in. Flat parts stay exactly at the surface.
	};

	constexpr int PRESET_COUNT = 5;
	const char* PresetName(Preset preset);
	Recipe RecipeFor(Preset preset);

	// `normal` holds the mips, largest first. Their red and green go into the result untouched, so the shading stays
	// what it was, whatever is in them. `color` can be any size.
	bool Build(const std::vector<Pixels>& normal, const Pixels& color, const Recipe& recipe, PomFile& out, std::string& error);

	// Works out the crevice shading from a file's height and stores it in alpha: 255 in the open, darker the further a
	// point sits below what's around it. Build does this itself. It's for .pom files made before there was any.
	void AddShading(PomFile& file);
	bool HasShading(const PomFile& file);
}

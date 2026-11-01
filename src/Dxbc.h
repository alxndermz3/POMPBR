#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Gives one of the game's lit pixel shaders parallax occlusion. All of them read layer 0 the same way:
//     sample r0.xyzw, vUv.xyxx, t0.xyzw, s0     color
//     sample r1.xyzw, vUv.xyxx, t1.xyzw, s1     normal
//     sample r2.xyzw, vUv.xyxx, t2.xyzw, s2     specular, where there is one
// The patch puts Prologue.hlsl in front of the first instruction and turns those into sample_d at the UV it found. Other
// reads of that UV use the new one too, like the camo's, which is worked out from it. The prologue's other results go
// where the shader's lighting can use them:
//   - The color sample is multiplied by the crevice shading.
//   - The sun's color (cb0[19]) or the primary light's (cb0[78]) is read from a temp with the relief shadow taken off.
//     Each shader reads it in exactly one place.
//   - Shadow lookups use the point the ray hit: TEXCOORD4, and the placed spot's own lookup from cb0[22..25].
//   - The results of the placed spot and omni blocks are multiplied by their relief shadow right after each block. Not
//     inside: MoreLights copies those blocks, and needs them as the game has them.
// Everything else is the game's own: fog, lightmap UVs, other layers.
namespace Pom::Dxbc
{
	// Same order as the cbuffer in Prologue.hlsl. They end up as literals in the shader.
	struct Literals
	{
		float depthUv = 0;
		float mostSteps = 32;
		float fewestSteps = 8;
		float steepest = 0.1f;
		float textureWidth = 256;
		float fadeEnd = 1100;
		float fadeScale = 1.0f / 400;
		float traceSteps = 0;
		float crevice = 0;
		float reliefShadows = 0;
		float pad0 = 0;
		float pad1 = 0;
	};

	struct Result
	{
		std::vector<uint8_t> shader;  // Empty unless ok.
		bool ok = false;
		std::string error;
		unsigned samples = 0;         // How many samples of layer 0 now use the shifted UV.
		unsigned light = 0;           // 0 none, 1 the sun, 2 a primary light.
		bool movedLookup = false;     // Its shadow lookup follows the relief.
		bool spotBlock = false;       // It has the placed spot's block, and takes relief shadows on it.
		bool omniBlock = false;
	};

	Result Patch(const uint8_t* shader, size_t size, const Literals& literals);
}

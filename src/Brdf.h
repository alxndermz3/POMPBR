#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Gives one of the game's lit pixel shaders GGX highlights, Smith visibility, anti-sparkle and better reflections. See
// Pbr.hlsl for the why. Everything is found by following what the game's code does with its registers, from values only
// that code has: the 13 in 2 ^ (13 * gloss), the -9.28 of the environment term, the -10 of Fresnel's. In one game shader:
//     dp3_sat NoV, N, -V                   mul X, NoV, l(-9.28)     which registers are the normal and the view, and from
//                                                                   them, which inputs they came from: TEXCOORDs vary
//     mul X, gloss, l(13)                  exp power, X             the power
//     log A, NoH   mul B, A, power         exp E, B                 the lobe, once per light that has one
//     add/mad Z, ..., l(1.01)              div V, NoL, Z            visibility, once per light
//     sample_l R, reflected, t15, s15, lod ...decode...             the probe
//     mul R, R, F   mad X, R, tint, rest                            the reflection on its way to the color
// Every part is optional: a shader gets what fits it, and one with none of it stays as it is. Nothing here depends on the
// parallax patch, which runs after this one when a surface has both, and sees the result as just another shader.
namespace Pom::Brdf
{
	// Same order as the cbuffer in Pbr.hlsl: bound at b12, by Reflect.cpp.
	struct Constants
	{
		float roughnessOffset = 0;
		float highlightsOffset = 0;
		float reflectionsOffset = 0;
		float screenAmount = 0;
		float screenWidth = 0;
		float screenHeight = 0;
		float screenSteps = 0;
		float screenReach = 0;
	};

	constexpr unsigned CONSTANT_SLOT = 12;
	constexpr unsigned SCENE_COLOR_SLOT = 16;  // Free in every shader, unlike t4 and t5: the layer shaders use those.
	constexpr unsigned SCENE_DEPTH_SLOT = 17;

	struct Result
	{
		std::vector<uint8_t> shader;  // Empty unless changed.
		bool ok = false;
		bool changed = false;
		std::string error;
		unsigned lobes = 0;           // Highlights now GGX.
		unsigned visibilities = 0;    // Of those, with Smith.
		bool widened = false;         // It has the anti-sparkle.
		bool probe = false;           // Its probe has the horizon fade.
		bool screen = false;          // It has screen reflections.
		bool omni = false;            // Its omni lights have highlights.
	};

	Result Patch(const uint8_t* shader, size_t size);
}

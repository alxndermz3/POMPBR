#pragma once

// The tool's quality setting: height samples per pixel, looking along a surface and straight at it, and for each light's
// relief shadow. Costs GPU time on POM surfaces only. Ultra samples the same at any angle.
namespace Pom
{
	struct QualityLevel
	{
		const char* name;
		float mostSteps;
		float fewestSteps;
		float traceSteps;
	};

	constexpr QualityLevel QUALITY_LEVELS[] = {
		{"Low", 12, 6, 6},
		{"Medium", 20, 10, 10},
		{"High", 32, 16, 16},
		{"Ultra", 64, 64, 32},
		{"Extreme", 128, 128, 64},
		{"Insane", 256, 256, 128},
	};

	constexpr int QUALITY_COUNT = 6;
	constexpr int QUALITY_DEFAULT = 2;
}

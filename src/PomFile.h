#pragma once
#include <cstdint>
#include <string>
#include <vector>

// A .pom file, one per native normal map, named after it: Plugins\POM\<image name>.pom. tools/bake_pom.py writes them.
// Behind the header: RGBA8 mips, largest first. Red and green are the native normal map, blue is height (255 = the
// surface itself, 0 = depthUv below it), alpha is unused.
namespace Pom
{
	struct PomHeader
	{
		char magic[4];  // "POM1"
		uint32_t width;
		uint32_t height;
		uint32_t mips;
		float depthUv;  // How deep height 0 is, in UV units. The ini's [Depth] scales this.
	};

	struct PomFile
	{
		PomHeader header{};
		std::vector<uint8_t> pixels;
	};

	bool ReadPomFile(const std::string& path, PomFile& out, std::string& error);
	bool WritePomFile(const std::string& path, const PomFile& file, std::string& error);
}

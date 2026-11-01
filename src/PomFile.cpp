#include "PomFile.h"

#include <Windows.h>

#include <cstdio>
#include <cstring>

namespace Pom
{
bool ReadPomFile(const std::string& path, PomFile& out, std::string& error)
{
	FILE* file = fopen(path.c_str(), "rb");
	if (!file)
	{
		error = "can't open it";
		return false;
	}
	bool ok = fread(&out.header, sizeof(out.header), 1, file) == 1;
	const PomHeader& h = out.header;
	if (!ok || memcmp(h.magic, "POM1", 4) != 0 || !h.width || !h.height || h.width > 8192 || h.height > 8192 || !h.mips ||
		h.mips > 14)
	{
		error = "not a POM1 file";
		fclose(file);
		return false;
	}
	size_t bytes = 0;
	uint32_t width = h.width, height = h.height;
	for (uint32_t i = 0; i < h.mips; ++i)
	{
		bytes += size_t(width) * height * 4;
		width = width > 1 ? width / 2 : 1;
		height = height > 1 ? height / 2 : 1;
	}
	out.pixels.resize(bytes);
	ok = fread(out.pixels.data(), 1, bytes, file) == bytes;
	fclose(file);
	if (!ok)
		error = "it's shorter than its header says";
	return ok;
}

bool WritePomFile(const std::string& path, const PomFile& file, std::string& error)
{
	// Written beside the real name first, so a game closing mid-write can't leave half a file for the next start.
	const std::string temporary = path + ".tmp";
	FILE* out = fopen(temporary.c_str(), "wb");
	if (!out)
	{
		error = "can't write " + path;
		return false;
	}
	const bool ok = fwrite(&file.header, sizeof(file.header), 1, out) == 1 &&
		fwrite(file.pixels.data(), 1, file.pixels.size(), out) == file.pixels.size();
	fclose(out);
	if (!ok || !MoveFileExA(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
	{
		remove(temporary.c_str());
		error = "can't write " + path;
		return false;
	}
	return true;
}
}

#include "Settings.h"

#include <Windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Log.h"

namespace Pom
{
namespace
{
	FILETIME g_LastWrite{};
	constexpr const char* SECTION = "POM";
	constexpr const char* DEPTH_SECTION = "Depth";
	constexpr const char* PBR_SECTION = "PBR";
	constexpr int DEFAULT_TOOL_KEY = VK_F7;

	std::string Path()
	{
		return Log::PluginDirectory() + "\\POM.ini";
	}

	float ReadFloat(const std::string& path, const char* section, const char* key, float fallback)
	{
		char text[32] = {};
		GetPrivateProfileStringA(section, key, "", text, sizeof(text), path.c_str());
		char* end = nullptr;
		const float value = float(strtod(text, &end));
		return end != text ? value : fallback;
	}

	// F1..F12, a single letter or digit, or a virtual key code.
	int ParseKey(const char* text)
	{
		if ((text[0] == 'F' || text[0] == 'f') && text[1])
		{
			const int number = atoi(text + 1);
			if (number >= 1 && number <= 12)
				return VK_F1 + number - 1;
		}
		if (text[0] && !text[1] && isalnum(static_cast<unsigned char>(text[0])))
			return toupper(static_cast<unsigned char>(text[0]));
		const int code = int(strtol(text, nullptr, 0));
		return code > 0 && code < 256 ? code : DEFAULT_TOOL_KEY;
	}

	// Our own writes aren't news: the next Reload would redo what the caller just did.
	void NoteOwnWrite()
	{
		WIN32_FILE_ATTRIBUTE_DATA data{};
		if (GetFileAttributesExA(Path().c_str(), GetFileExInfoStandard, &data))
			g_LastWrite = data.ftLastWriteTime;
	}
}

void SettingsFile::EnsureExists()
{
	const std::string path = Path();
	if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES)
		return;
	if (FILE* f = fopen(path.c_str(), "w"))
	{
		fputs("; POM: parallax occlusion on the textures that have a .pom file in Plugins\\POM, and nothing else.\n"
			  "; Saved changes apply within a second, no restart. CTRL+F8 toggles Enabled.\n"
			  "[POM]\n"
			  "Enabled=1\n"
			  "; Opens the picker: freeze the camera, click a surface, Convert to POM. F1..F12, a letter, or a key code.\n"
			  "ToolKey=F7\n"
			  "; Ultra, High, Medium or Low. Costs GPU time on POM surfaces only. The tool sets this too.\n"
			  "Quality=High\n"
			  "; Keeps grazing views from sliding far: the slant used is at most that of this view cosine.\n"
			  "Steepest=0.1\n"
			  "; World units from the camera where the effect starts fading and where it's gone. The tool sets these too.\n"
			  "FadeStart=700\n"
			  "FadeEnd=1100\n"
			  "; How much the gaps darken, and how much the relief shadows itself from each light, 0 to 1. The tool sets these too.\n"
			  "Crevices=0.6\n"
			  "ReliefShadows=1\n"
			  "\n"
			  "; Physically based highlights and reflections on everything lit. CTRL+F7 toggles Enabled. The tool sets the rest too.\n"
			  "[PBR]\n"
			  "Enabled=1\n"
			  "Highlights=1\n"
			  "Roughness=1\n"
			  "Reflections=1\n"
			  "; 0 leaves reflections to the game's low resolution probes.\n"
			  "ScreenReflections=1\n"
			  "; Samples per reflection at most, and how far they look in world units.\n"
			  "ScreenSteps=32\n"
			  "ScreenReach=1500\n"
			  "\n"
			  "; Multiplies a texture's depth, by image name. 0 = flat, below 0 = inverted. The tool writes these too.\n"
			  "[Depth]\n", f);
		fclose(f);
		Log::Write("wrote default settings: %s", path.c_str());
	}
}

bool SettingsFile::Reload(Settings& out)
{
	const std::string path = Path();
	WIN32_FILE_ATTRIBUTE_DATA data{};
	if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data))
		return false;
	if (CompareFileTime(&data.ftLastWriteTime, &g_LastWrite) == 0)
		return false;
	g_LastWrite = data.ftLastWriteTime;

	out.enabled = GetPrivateProfileIntA(SECTION, "Enabled", 1, path.c_str()) != 0;
	char quality[16] = {};
	GetPrivateProfileStringA(SECTION, "Quality", "", quality, sizeof(quality), path.c_str());
	out.quality = QUALITY_DEFAULT;
	for (int i = 0; i < QUALITY_COUNT; ++i)
	{
		if (_stricmp(quality, QUALITY_LEVELS[i].name) == 0)
			out.quality = i;
	}
	out.steepest = ReadFloat(path, SECTION, "Steepest", 0.1f);
	out.fadeStart = ReadFloat(path, SECTION, "FadeStart", 700);
	out.fadeEnd = ReadFloat(path, SECTION, "FadeEnd", 1100);
	out.crevice = ReadFloat(path, SECTION, "Crevices", 0.6f);
	out.reliefShadows = ReadFloat(path, SECTION, "ReliefShadows", 1);
	char key[16] = {};
	GetPrivateProfileStringA(SECTION, "ToolKey", "F7", key, sizeof(key), path.c_str());
	out.toolKey = ParseKey(key);
	PbrSettings& pbr = out.pbr;
	pbr.enabled = GetPrivateProfileIntA(PBR_SECTION, "Enabled", 1, path.c_str()) != 0;
	pbr.highlights = ReadFloat(path, PBR_SECTION, "Highlights", 1);
	pbr.roughness = ReadFloat(path, PBR_SECTION, "Roughness", 1);
	pbr.reflections = ReadFloat(path, PBR_SECTION, "Reflections", 1);
	pbr.screenReflections = ReadFloat(path, PBR_SECTION, "ScreenReflections", 1);
	pbr.screenSteps = GetPrivateProfileIntA(PBR_SECTION, "ScreenSteps", 32, path.c_str());
	pbr.screenReach = ReadFloat(path, PBR_SECTION, "ScreenReach", 1500);
	Log::Write("settings: PBR %d, highlights %.2f, roughness %.2f, reflections %.2f, screen %.2f, %d steps, reach %.0f", pbr.enabled, pbr.highlights, pbr.roughness, pbr.reflections,
		pbr.screenReflections, pbr.screenSteps, pbr.screenReach);
	Log::Write("settings: enabled %d, quality %s, steepest %.2f, fade %.0f..%.0f, crevices %.2f, relief shadows %.2f, tool key %s", out.enabled,
		QUALITY_LEVELS[out.quality].name, out.steepest, out.fadeStart, out.fadeEnd, out.crevice, out.reliefShadows, KeyName(out.toolKey));
	return true;
}

void SettingsFile::SaveEnabled(bool enabled)
{
	WritePrivateProfileStringA(SECTION, "Enabled", enabled ? "1" : "0", Path().c_str());
}

void SettingsFile::SaveLook(int quality, float fadeStart, float fadeEnd, float crevice, float reliefShadows)
{
	const std::string path = Path();
	WritePrivateProfileStringA(SECTION, "Quality", QUALITY_LEVELS[quality].name, path.c_str());
	const auto save = [&](const char* key, const char* format, float value)
	{
		char text[32];
		snprintf(text, sizeof(text), format, value);
		WritePrivateProfileStringA(SECTION, key, text, path.c_str());
	};
	save("FadeStart", "%.0f", fadeStart);
	save("FadeEnd", "%.0f", fadeEnd);
	save("Crevices", "%.2f", crevice);
	save("ReliefShadows", "%.2f", reliefShadows);
	NoteOwnWrite();
}

void SettingsFile::SavePbr(const PbrSettings& pbr)
{
	const std::string path = Path();
	const auto save = [&](const char* key, float value)
	{
		char text[32];
		snprintf(text, sizeof(text), "%.3f", value);
		WritePrivateProfileStringA(PBR_SECTION, key, text, path.c_str());
	};
	WritePrivateProfileStringA(PBR_SECTION, "Enabled", pbr.enabled ? "1" : "0", path.c_str());
	save("Highlights", pbr.highlights);
	save("Roughness", pbr.roughness);
	save("Reflections", pbr.reflections);
	save("ScreenReflections", pbr.screenReflections);
	save("ScreenReach", pbr.screenReach);
	WritePrivateProfileStringA(PBR_SECTION, "ScreenSteps", std::to_string(pbr.screenSteps).c_str(), path.c_str());
	NoteOwnWrite();
}

float SettingsFile::DepthScale(const std::string& image)
{
	return ReadFloat(Path(), DEPTH_SECTION, image.c_str(), 1);
}

void SettingsFile::SetDepthScale(const std::string& image, float scale)
{
	char text[32];
	snprintf(text, sizeof(text), "%.3f", scale);
	WritePrivateProfileStringA(DEPTH_SECTION, image.c_str(), text, Path().c_str());
	NoteOwnWrite();
}

void SettingsFile::ForgetDepth(const std::string& image)
{
	WritePrivateProfileStringA(DEPTH_SECTION, image.c_str(), nullptr, Path().c_str());
	NoteOwnWrite();
}

std::string SettingsFile::TextureFolder()
{
	return Log::PluginDirectory() + "\\POM";
}

const char* SettingsFile::KeyName(int key)
{
	static char name[8];
	if (key >= VK_F1 && key <= VK_F12)
		snprintf(name, sizeof(name), "F%d", key - VK_F1 + 1);
	else if (key >= '0' && key <= 'Z')
		snprintf(name, sizeof(name), "%c", key);
	else
		snprintf(name, sizeof(name), "0x%02X", key);
	return name;
}
}

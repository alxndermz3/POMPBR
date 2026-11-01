#pragma once
#include <string>

#include "Quality.h"

namespace Pom
{
	// [PBR] in POM.ini. All of them apply at once: they're constants, not part of the shaders.
	struct PbrSettings
	{
		bool enabled = true;
		float highlights = 1;         // How bright the highlights of lights are.
		float roughness = 1;          // Scales how wide they are. Authored gloss is 1.
		float reflections = 1;        // How strong the probe's and the screen's reflections are.
		float screenReflections = 1;  // How much the screen takes over from the probe, 0 = none.
		int screenSteps = 32;         // Samples per reflection at most.
		float screenReach = 1500;     // How far a reflection looks, in world units.
	};

	struct Settings
	{
		bool enabled = true;
		int quality = QUALITY_DEFAULT;
		float steepest = 0.1f;
		float fadeStart = 700;
		float fadeEnd = 1100;
		float crevice = 0.6f;
		float reliefShadows = 1;
		int toolKey = 0x76;  // VK_F7
		PbrSettings pbr;
	};

	// POM.ini next to the plugin; saved changes apply within a second.
	namespace SettingsFile
	{
		void EnsureExists();
		bool Reload(Settings& out);
		void SaveEnabled(bool enabled);
		void SaveLook(int quality, float fadeStart, float fadeEnd, float crevice, float reliefShadows);
		void SavePbr(const PbrSettings& pbr);
		float DepthScale(const std::string& image);  // [Depth] <image name>=<multiplier>, 1 if it isn't there.
		void SetDepthScale(const std::string& image, float scale);
		void ForgetDepth(const std::string& image);
		std::string TextureFolder();                 // Plugins\POM
		const char* KeyName(int key);
	}
}

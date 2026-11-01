#pragma once
#include <string>
#include <vector>

// The tool's window. Only ImGui: it's told what to show and says what was pressed, so it can be drawn without the game.
namespace Pom::Panel
{
	struct Item
	{
		std::string image;
		unsigned materials = 0;
	};

	struct Model
	{
		std::string keyName;
		bool selected = false;
		std::string material, normalImage, refused;
		unsigned sharing = 0;
		unsigned width = 0, height = 0;
		bool converted = false;
		float depthScale = 1;
		int preset = 0;
		bool busy = false;
		std::string status;
		bool statusIsBad = false;
		std::vector<Item> list;
		int quality = 0;
		float fadeStart = 0, fadeEnd = 0;
		float crevice = 0, reliefShadows = 0;
		bool pbr = false;
		float highlights = 1, roughness = 1, reflections = 1, screenReflections = 1;
		int screenSteps = 32;
		float screenReach = 1500;
	};

	struct Pressed
	{
		bool convert = false;    // Convert, or bake again.
		bool remove = false;     // The selected surface's POM.
		bool close = false;
		bool depthChanged = false;
		bool depthReleased = false;
		float depthScale = 1;     // How much: Model::depthScale's sign is what says inverted.
		bool invertToggled = false;
		bool presetChanged = false;
		int preset = 0;
		std::string removeImage;  // A row of the list.
		bool lookChanged = false; // Quality, fade or shadows moved...
		bool lookReleased = false;  // ...and was let go of.
		int quality = 0;
		float fadeStart = 0, fadeEnd = 0;
		float crevice = 0, reliefShadows = 0;
		bool pbrChanged = false;    // PBR moved, which shows straight away...
		bool pbrReleased = false;   // ...and was let go of, which is when it's kept.
		bool pbrToggled = false;
		bool pbr = false;
		float highlights = 1, roughness = 1, reflections = 1, screenReflections = 1;
		int screenSteps = 32;
		float screenReach = 1500;
	};

	void Style();
	void Draw(const Model& model, Pressed& pressed);
}

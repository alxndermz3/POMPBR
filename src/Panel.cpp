#include "Panel.h"

#include <cmath>
#include <cstdio>

#include "Bake.h"
#include "Quality.h"
#include "imgui.h"

namespace Pom::Panel
{
namespace
{
	const ImVec4 ACCENT(0.91f, 0.64f, 0.24f, 1.0f);
	const ImVec4 ACCENT_HOVER(0.98f, 0.73f, 0.33f, 1.0f);
	const ImVec4 ACCENT_DOWN(0.80f, 0.54f, 0.18f, 1.0f);
	const ImVec4 DIM(0.62f, 0.64f, 0.67f, 1.0f);
	const ImVec4 BAD(0.96f, 0.47f, 0.40f, 1.0f);
	constexpr float WINDOW_WIDTH = 390;
	constexpr float WINDOW_MARGIN = 24;
	constexpr float LIST_ROWS_MAX = 6;
	// How far a slider drags. CTRL+click on one types any number at all.
	constexpr float DEPTH_MAX = 100;
	constexpr float FADE_MAX = 50000;
	constexpr float CREVICE_MAX = 5;
	constexpr float RELIEF_MAX = 10;
	constexpr float FADE_GAP = 1;
	constexpr float HINT_SIZE = 13;
	constexpr float HINT_ALPHA = 0.45f;
	constexpr float HIGHLIGHTS_MAX = 50;
	constexpr float ROUGHNESS_MIN = 0.01f;
	constexpr float ROUGHNESS_MAX = 100;
	constexpr float REFLECTIONS_MAX = 20;
	constexpr float SCREEN_MAX = 5;
	constexpr int SCREEN_STEPS_MIN = 8;
	constexpr int SCREEN_STEPS_MAX = 512;
	constexpr float SCREEN_REACH_MIN = 100;
	constexpr float SCREEN_REACH_MAX = 50000;

	void Label(const char* text)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, DIM);
		ImGui::TextUnformatted(text);
		ImGui::PopStyleColor();
	}

	void Wrapped(const std::string& text, const ImVec4& color)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, color);
		ImGui::PushTextWrapPos(0);
		ImGui::TextUnformatted(text.c_str());
		ImGui::PopTextWrapPos();
		ImGui::PopStyleColor();
	}

	bool PrimaryButton(const char* label, float width, bool enabled)
	{
		ImGui::BeginDisabled(!enabled);
		ImGui::PushStyleColor(ImGuiCol_Button, ACCENT);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ACCENT_HOVER);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ACCENT_DOWN);
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.09f, 0.08f, 0.06f, 1));
		const bool pressed = ImGui::Button(label, ImVec2(width, 38));
		ImGui::PopStyleColor(4);
		ImGui::EndDisabled();
		return pressed;
	}

	void DrawSelection(const Model& model, Pressed& pressed)
	{
		Label("Material");
		Wrapped(model.material, ImVec4(1, 1, 1, 1));
		ImGui::Spacing();
		Label("Normal map");
		std::string normal = model.normalImage.empty() ? "none" : model.normalImage;
		if (model.width)
			normal += "  (" + std::to_string(model.width) + " x " + std::to_string(model.height) + ")";
		Wrapped(normal, ImVec4(1, 1, 1, 1));
		if (model.sharing > 1)
			Wrapped(std::to_string(model.sharing) + " materials use this normal map. They all change together; the highlight shows them.", DIM);

		ImGui::Spacing();
		if (!model.refused.empty())
		{
			Wrapped(model.refused, BAD);
			return;
		}

		const float width = ImGui::GetContentRegionAvail().x;
		Label("Surface type");
		ImGui::SetNextItemWidth(width);
		int preset = model.preset;
		ImGui::BeginDisabled(model.busy);
		if (ImGui::BeginCombo("##preset", Bake::PresetName(Bake::Preset(preset))))
		{
			for (int i = 0; i < Bake::PRESET_COUNT; ++i)
			{
				if (ImGui::Selectable(Bake::PresetName(Bake::Preset(i)), i == preset))
				{
					pressed.presetChanged = i != preset;
					pressed.preset = i;
				}
			}
			ImGui::EndCombo();
		}
		ImGui::EndDisabled();

		ImGui::Spacing();
		if (model.converted)
		{
			Label("Depth");
			ImGui::SetNextItemWidth(width);
			float depth = std::fabs(model.depthScale);
			ImGui::BeginDisabled(model.busy);
			if (ImGui::SliderFloat("##depth", &depth, 0, DEPTH_MAX, "%.2f x", ImGuiSliderFlags_Logarithmic))
			{
				pressed.depthChanged = true;
				pressed.depthScale = depth;
			}
			pressed.depthReleased = ImGui::IsItemDeactivatedAfterEdit();
			if (pressed.depthReleased)
				pressed.depthScale = depth;
			bool inverted = std::signbit(model.depthScale);
			pressed.invertToggled = ImGui::Checkbox("Invert", &inverted);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Carved parts stand out instead. Flat parts stay where they are.");
			ImGui::EndDisabled();
			ImGui::Spacing();
			const float half = (width - ImGui::GetStyle().ItemSpacing.x) / 2;
			ImGui::BeginDisabled(model.busy);
			pressed.convert = ImGui::Button("Bake again", ImVec2(half, 38));
			ImGui::SameLine();
			pressed.remove = ImGui::Button("Remove POM", ImVec2(half, 38));
			ImGui::EndDisabled();
		}
		else
		{
			pressed.convert = PrimaryButton(model.busy ? "Converting..." : "Convert to POM", width, !model.busy);
		}
	}

	// For every texture on POM, whether or not anything is selected. Remaking the shaders takes a moment, so it's asked for
	// when a slider is let go of.
	void DrawLook(const Model& model, Pressed& pressed)
	{
		const float width = ImGui::GetContentRegionAvail().x;
		int quality = model.quality;
		float start = model.fadeStart, end = model.fadeEnd;

		Label("Quality");
		ImGui::SetNextItemWidth(width);
		if (ImGui::SliderInt("##quality", &quality, 0, QUALITY_COUNT - 1, QUALITY_LEVELS[quality].name, ImGuiSliderFlags_NoInput))
			pressed.lookChanged = true;
		pressed.lookReleased |= ImGui::IsItemDeactivatedAfterEdit();

		Label("Fade out distance");
		const float half = (width - ImGui::GetStyle().ItemSpacing.x) / 2;
		ImGui::SetNextItemWidth(half);
		if (ImGui::SliderFloat("##fadeStart", &start, 0, FADE_MAX, "starts at %.0f", ImGuiSliderFlags_Logarithmic))
			pressed.lookChanged = true;
		pressed.lookReleased |= ImGui::IsItemDeactivatedAfterEdit();
		ImGui::SameLine();
		ImGui::SetNextItemWidth(half);
		if (ImGui::SliderFloat("##fadeEnd", &end, 0, FADE_MAX, "gone at %.0f", ImGuiSliderFlags_Logarithmic))
			pressed.lookChanged = true;
		pressed.lookReleased |= ImGui::IsItemDeactivatedAfterEdit();

		Label("Shadows");
		float crevice = model.crevice, shadows = model.reliefShadows;
		ImGui::SetNextItemWidth(half);
		if (ImGui::SliderFloat("##crevice", &crevice, 0, CREVICE_MAX, "crevices %.2f"))
			pressed.lookChanged = true;
		pressed.lookReleased |= ImGui::IsItemDeactivatedAfterEdit();
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("How much gaps darken, in any light.");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(half);
		if (ImGui::SliderFloat("##shadows", &shadows, 0, RELIEF_MAX, "relief %.2f"))
			pressed.lookChanged = true;
		pressed.lookReleased |= ImGui::IsItemDeactivatedAfterEdit();
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("How much the relief shadows itself from the sun and lights, and shadow edges follow it.");
		pressed.crevice = crevice;
		pressed.reliefShadows = shadows;

		// Whichever was dragged pushes the other along: the two can't cross.
		if (start > end - FADE_GAP)
		{
			if (start != model.fadeStart)
				end = start + FADE_GAP;
			else
				start = (std::fmax)(end - FADE_GAP, 0.0f);
		}
		pressed.quality = quality;
		pressed.fadeStart = start;
		pressed.fadeEnd = end;
	}

	// These are constants the shaders read, so unlike the rest they show as a slider moves.
	void DrawPbr(const Model& model, Pressed& pressed)
	{
		pressed.pbr = model.pbr;
		pressed.highlights = model.highlights;
		pressed.roughness = model.roughness;
		pressed.reflections = model.reflections;
		pressed.screenReflections = model.screenReflections;
		pressed.screenSteps = model.screenSteps;
		pressed.screenReach = model.screenReach;

		pressed.pbrToggled = ImGui::Checkbox("PBR", &pressed.pbr);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Real highlights and reflections on everything lit, as authored in the game's own gloss and spec maps.");

		ImGui::BeginDisabled(!model.pbr);
		const float width = ImGui::GetContentRegionAvail().x;
		const float half = (width - ImGui::GetStyle().ItemSpacing.x) / 2;
		const auto slider = [&](const char* id, float* value, float low, float high, const char* format, ImGuiSliderFlags flags, const char* tip)
		{
			ImGui::SetNextItemWidth(half);
			if (ImGui::SliderFloat(id, value, low, high, format, flags))
				pressed.pbrChanged = true;
			pressed.pbrReleased |= ImGui::IsItemDeactivatedAfterEdit();
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", tip);
		};
		slider("##highlights", &pressed.highlights, 0, HIGHLIGHTS_MAX, "highlights %.2f", ImGuiSliderFlags_Logarithmic, "How bright lights show on surfaces.");
		ImGui::SameLine();
		slider("##roughness", &pressed.roughness, ROUGHNESS_MIN, ROUGHNESS_MAX, "width %.2f", ImGuiSliderFlags_Logarithmic, "How wide highlights spread. 1 is as the game's textures say.");
		slider("##reflections", &pressed.reflections, 0, REFLECTIONS_MAX, "reflections %.2f", ImGuiSliderFlags_Logarithmic, "How strong reflections are.");
		ImGui::SameLine();
		slider("##screen", &pressed.screenReflections, 0, SCREEN_MAX, "on screen %.2f", 0, "How much what's really on screen replaces the game's blurry reflection maps. Past 1 it gets stronger.");
		ImGui::SetNextItemWidth(half);
		if (ImGui::SliderInt("##steps", &pressed.screenSteps, SCREEN_STEPS_MIN, SCREEN_STEPS_MAX, "%d steps", ImGuiSliderFlags_Logarithmic))
			pressed.pbrChanged = true;
		pressed.pbrReleased |= ImGui::IsItemDeactivatedAfterEdit();
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Samples per reflection at most. More finds thin things, and costs GPU time on glossy surfaces.");
		ImGui::SameLine();
		slider("##reach", &pressed.screenReach, SCREEN_REACH_MIN, SCREEN_REACH_MAX, "reach %.0f", ImGuiSliderFlags_Logarithmic, "How far away reflected things can be, in world units.");
		ImGui::EndDisabled();
	}

	void DrawList(const Model& model, Pressed& pressed)
	{
		char title[64];
		snprintf(title, sizeof(title), "Converted textures (%u)###list", unsigned(model.list.size()));
		if (!ImGui::CollapsingHeader(title))
			return;
		if (model.list.empty())
		{
			Label("None on this map yet.");
			return;
		}
		const float row = ImGui::GetFrameHeightWithSpacing();
		const float rows = model.list.size() < LIST_ROWS_MAX ? float(model.list.size()) : LIST_ROWS_MAX;
		ImGui::BeginChild("##rows", ImVec2(0, rows * row + 4));
		for (const Item& item : model.list)
		{
			ImGui::PushID(item.image.c_str());
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(item.image.c_str());
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%u materials", item.materials);
			ImGui::SameLine(ImGui::GetContentRegionMax().x - 26);
			ImGui::BeginDisabled(model.busy);
			if (ImGui::SmallButton("x"))
				pressed.removeImage = item.image;
			ImGui::EndDisabled();
			ImGui::PopID();
		}
		ImGui::EndChild();
	}
}

void Style()
{
	ImGuiStyle& style = ImGui::GetStyle();
	style.WindowRounding = 8;
	style.FrameRounding = 5;
	style.GrabRounding = 5;
	style.PopupRounding = 6;
	style.WindowPadding = ImVec2(16, 14);
	style.FramePadding = ImVec2(10, 7);
	style.ItemSpacing = ImVec2(8, 6);
	style.WindowBorderSize = 1;

	ImVec4* colors = style.Colors;
	colors[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.10f, 0.11f, 0.94f);
	colors[ImGuiCol_PopupBg] = ImVec4(0.11f, 0.12f, 0.13f, 0.98f);
	colors[ImGuiCol_Border] = ImVec4(1, 1, 1, 0.10f);
	colors[ImGuiCol_FrameBg] = ImVec4(1, 1, 1, 0.07f);
	colors[ImGuiCol_FrameBgHovered] = ImVec4(1, 1, 1, 0.12f);
	colors[ImGuiCol_FrameBgActive] = ImVec4(1, 1, 1, 0.16f);
	colors[ImGuiCol_Button] = ImVec4(1, 1, 1, 0.09f);
	colors[ImGuiCol_ButtonHovered] = ImVec4(1, 1, 1, 0.16f);
	colors[ImGuiCol_ButtonActive] = ImVec4(1, 1, 1, 0.22f);
	colors[ImGuiCol_Header] = ImVec4(1, 1, 1, 0.07f);
	colors[ImGuiCol_HeaderHovered] = ImVec4(1, 1, 1, 0.13f);
	colors[ImGuiCol_HeaderActive] = ImVec4(1, 1, 1, 0.18f);
	colors[ImGuiCol_SliderGrab] = ACCENT;
	colors[ImGuiCol_SliderGrabActive] = ACCENT_HOVER;
	colors[ImGuiCol_CheckMark] = ACCENT;
	colors[ImGuiCol_Separator] = ImVec4(1, 1, 1, 0.10f);
}

void Draw(const Model& model, Pressed& pressed)
{
	const ImGuiIO& io = ImGui::GetIO();
	ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - WINDOW_WIDTH - WINDOW_MARGIN, WINDOW_MARGIN));
	ImGui::SetNextWindowSizeConstraints(ImVec2(WINDOW_WIDTH, 0), ImVec2(WINDOW_WIDTH, io.DisplaySize.y - WINDOW_MARGIN * 2));
	ImGui::Begin("POM", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_AlwaysAutoResize);

	ImGui::PushStyleColor(ImGuiCol_Text, ACCENT);
	ImGui::TextUnformatted("POM");
	ImGui::PopStyleColor();
	ImGui::SameLine();
	Label("Camera frozen");
	ImGui::SameLine(ImGui::GetContentRegionMax().x - 54);
	pressed.close = ImGui::SmallButton("Close");
	Label(("Click any surface to select it. " + model.keyName + " or Esc to leave.").c_str());
	ImGui::Separator();
	ImGui::Spacing();

	if (model.selected)
		DrawSelection(model, pressed);
	else
		Wrapped("Nothing selected.", DIM);

	if (!model.status.empty())
	{
		ImGui::Spacing();
		Wrapped(model.status, model.statusIsBad ? BAD : ACCENT);
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();
	DrawLook(model, pressed);

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();
	DrawPbr(model, pressed);

	ImGui::Spacing();
	DrawList(model, pressed);

	ImGui::Spacing();
	ImGui::PushFont(nullptr, HINT_SIZE);
	ImGui::PushStyleVar(ImGuiStyleVar_Alpha, HINT_ALPHA);
	ImGui::TextUnformatted("CTRL+F8 Disables POM   CTRL+F7 Disables PBR");
	ImGui::TextUnformatted("CTRL+click any slider to type any number");
	ImGui::PopStyleVar();
	ImGui::PopFont();
	ImGui::End();
}
}

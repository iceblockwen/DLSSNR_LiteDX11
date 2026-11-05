// SPDX-License-Identifier: GPL-3.0-or-later
// The panel's ImGui widgets: tooltips, the labelled sliders with a numeric-input
// popup, and the reset control. Standalone: no panel state, no GPU objects.
#pragma once
#include <imgui.h>
#include <string>

namespace lite::panel_ui
{
void ItemTip(const char* text);
std::string Utf8(const std::wstring& value);
void SliderHelp(const char* label, double low, double high);
bool ResetValue(float& value, float initial);
void SliderResetWidth(const char* label);
bool PanelSliderFloat(const char* label, float* value, float low, float high,
                      const char* format = "%.3f", ImGuiSliderFlags flags = 0,
                      bool* finished = nullptr);
bool PanelSliderInt(const char* label, int* value, int low, int high);
}

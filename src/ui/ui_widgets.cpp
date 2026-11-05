// SPDX-License-Identifier: GPL-3.0-or-later
// The panel's ImGui widgets: tooltips, the labelled sliders with a numeric-input
// popup, and the reset control. Standalone: no panel state, no GPU objects.
#include "ui_widgets.hpp"
#include <windows.h>
#include <algorithm>
#include <cstring>

namespace lite::panel_ui
{
void ItemTip(const char* text)
{
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        return;
    }
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}
std::string Utf8(const std::wstring& value)
{
    if (value.empty())
    {
        return {};
    }
    const auto size = WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), nullptr, 0,
                                          nullptr, nullptr);
    std::string result(size, '\0');
    if (size)
    {
        WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), result.data(), size,
                            nullptr, nullptr);
    }
    return result;
}
const char* SliderTip(const char* label)
{
    struct Tip
    {
        const char* label;
        const char* text;
    };
    static const Tip tips[]{
        {"Scale",
         "Motion-vector preview scale. Does not change the vectors sent to NR. Default: 1."},
        {"Intensity",
         "Overall model effect strength. Applies when editing ends, rebuilding NR histories once. Default: 1."},
        {"Local structure",
         "Model strength for local detail and structure. Applies when editing ends, rebuilding NR histories once. Default: 1."},
        {"Local tone",
         "Model strength for local tonal changes. Applies when editing ends, rebuilding NR histories once. Default: 1."},
        {"Skin structure",
         "Skin detail strength. -1 follows Local structure; 0 or above sets it independently. Applies when editing ends, rebuilding NR histories once. Default: -1."},
        {"Difference gain",
         "Original + Gain * (NR - Original). 0 shows original, 1 normal NR, 2 doubles the change. Default: 1."},
        {"Manual / fallback",
         "Reference white in Manual mode, or the initial fallback if game exposure or input measurement is unavailable. Display-referred input always uses white 1. Default: 1."},
        {"White trim",
         "Multiplier on game or measured white. Range: 0.001 to 1024; Ctrl+click or right-click to type. Does not change Manual mode or display-input bypass. Default: 1."},
        {"Mid-gray anchor",
         "Measured white = input log-average / anchor * trim. A larger anchor lowers the white point. Applies to Measured input. Default: 0.18."},
        {"Filter seconds",
         "Time constant for measured-white smoothing. Larger values respond more slowly. Applies to Measured input. Default: 0.3 seconds."},
        {"Deadband",
         "Ignore relative target changes smaller than this fraction. 0.08 means 8%. Applies to Measured input. Default: 0.08."},
        {"Attack (stops/s)",
         "Maximum measured-white rise rate, in powers of two per second. 1 allows up to 2x per second; 0 holds upward changes. Default: 1."},
        {"Release (stops/s)",
         "Maximum measured-white fall rate, in powers of two per second. 1 allows down to half per second; 0 holds downward changes. Default: 0.5."},
        {"Measure every N frames",
         "Sampling interval in processed NR frames. Smaller values update measured white and exposure readback more often, with more sampling cost. Default: 4."}};
    for (const auto& tip : tips)
    {
        if (strcmp(label, tip.label) == 0)
        {
            return tip.text;
        }
    }
    return "";
}
void SliderHelp(const char* label, double low, double high)
{
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        return;
    }
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32);
    ImGui::TextUnformatted(SliderTip(label));
    ImGui::Separator();
    ImGui::Text("Range: %.9g to %.9g", low, high);
    ImGui::TextUnformatted(
        "Ctrl+click to type a value, or right-click for numeric input. Tab selects a control; arrow keys adjust it.");
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}
bool ResetValue(float& value, float initial)
{
    const bool clicked = ImGui::SmallButton("Reset");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Restore default: %.9g", double(initial));
    }
    if (clicked)
    {
        value = initial;
    }
    return clicked;
}
void SliderResetWidth(const char* label)
{
    const auto& style = ImGui::GetStyle();
    const float reserved = ImGui::CalcTextSize(label).x + ImGui::CalcTextSize("Reset").x +
                           2 * style.FramePadding.x + 3 * style.ItemSpacing.x;
    ImGui::SetNextItemWidth((std::max)(60.f, ImGui::GetContentRegionAvail().x - reserved));
}
bool PanelSliderFloat(const char* label, float* value, float low, float high,
                      const char* format, ImGuiSliderFlags flags, bool* finished)
{
    bool changed =
        ImGui::SliderFloat(label, value, low, high, format, flags | ImGuiSliderFlags_AlwaysClamp);
    bool done = ImGui::IsItemDeactivatedAfterEdit();
    SliderHelp(label, low, high);
    ImGui::PushID(label);
    // Capture the parent window's storage before BeginPopup changes the current
    // window. The scoped ID keeps independent sliders and panels separate.
    auto* storage = ImGui::GetStateStorage();
    const auto popupState = ImGui::GetID("##NumericPopupWasOpen");
    const bool wasOpen = storage->GetBool(popupState, false);
    if (ImGui::BeginPopupContextItem("Numeric input"))
    {
        ImGui::TextUnformatted(label);
        ImGui::TextDisabled("Enter a value (%.9g to %.9g)", double(low), double(high));
        ImGui::SetNextItemWidth(200);
        if (ImGui::IsWindowAppearing())
        {
            ImGui::SetKeyboardFocusHere();
        }
        if (ImGui::InputFloat("##Value", value, 0, 0, "%.9g"))
        {
            *value = (std::clamp)(*value, low, high);
            changed = true;
        }
        done |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::EndPopup();
    }
    // Closing a numeric popup removes its input item before deactivation can
    // be queried. Treat that close as completion of any pending edit.
    const bool popupOpen = ImGui::IsPopupOpen("Numeric input");
    done |= wasOpen && !popupOpen;
    if (wasOpen != popupOpen)
    {
        storage->SetBool(popupState, popupOpen);
    }
    ImGui::PopID();
    if (finished)
    {
        *finished = done;
    }
    return changed;
}
bool PanelSliderInt(const char* label, int* value, int low, int high)
{
    bool changed = ImGui::SliderInt(label, value, low, high, "%d", ImGuiSliderFlags_AlwaysClamp);
    SliderHelp(label, low, high);
    ImGui::PushID(label);
    if (ImGui::BeginPopupContextItem("Numeric input"))
    {
        ImGui::TextUnformatted(label);
        ImGui::TextDisabled("Enter a value (%d to %d)", low, high);
        ImGui::SetNextItemWidth(200);
        if (ImGui::IsWindowAppearing())
        {
            ImGui::SetKeyboardFocusHere();
        }
        if (ImGui::InputInt("##Value", value, 0, 0))
        {
            *value = (std::clamp)(*value, low, high);
            changed = true;
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return changed;
}
}

// SPDX-License-Identifier: GPL-3.0-or-later
// The panel's ImGui window: the settings pages and the status readouts.
#include "panel_impl.hpp"
#include "ui_widgets.hpp"
#include "../input/input.hpp"
#include "../config.hpp"
#include "../nr/depth11.hpp"
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <algorithm>

namespace lite
{
using namespace panel_ui;

void Panel::Impl::CommitFlowOptions()
{
    if (std::none_of(flowPending.begin(), flowPending.end(),
                     [](bool pending)
                     {
                         return pending;
                     }))
    {
        return;
    }
    auto settings = nr::FlowOptions();
    float* values[]{&settings.fov, &settings.farPlane, &settings.depthMultiplier};
    for (unsigned n = 0; n < 3; ++n)
    {
        if (flowPending[n])
        {
            *values[n] = flowEditing[n];
        }
    }
    flowPending.fill(false);
    nr::SetFlowOptions(settings);
}

bool Panel::Impl::CommitTuning(nr::ModelSettings* target)
{
    if (std::none_of(tuningPending.begin(), tuningPending.end(), [](bool p)
                     {
                         return p;
                     }))
    {
        return false;
    }
    auto settings = target ? *target : nr::GetModelSettings();
    float* values[]{&settings.pass.intensity, &settings.pass.structure, &settings.pass.tone,
                    &settings.pass.skin};
    for (unsigned n = 0; n < 4; ++n)
    {
        if (tuningPending[n])
        {
            *values[n] = tuningValues[n];
        }
    }
    tuningPending.fill(false);
    if (target)
    {
        *target = settings;
    }
    else
    {
        nr::SetModelSettings(settings);
    }
    return true;
}

void Panel::Impl::BuildResourceUi(float scale)
{
    const auto resources = nr::PreviewSnapshot();
    const auto pass = nr::LastPass();
    if (FAILED(resources.error))
    {
        ImGui::TextWrapped("%s (0x%08X)", Utf8(resources.reason).c_str(),
                           unsigned(resources.error));
    }
    const bool wide = ImGui::GetContentRegionAvail().x >= 450 * scale;
    if (ImGui::BeginTable("All resource previews", wide ? 3 : 1,
                          ImGuiTableFlags_SizingStretchSame))
    {
        const char* labels[]{"Color", "Depth", "Motion vectors"};
        for (unsigned n = 0; n < 3; ++n)
        {
            ImGui::TableNextColumn();
            ImGui::PushID(int(n));
            ImGui::Separator();
            ImGui::TextUnformatted(labels[n]);
            const auto r = resources.resources[n];
            if (r.present)
            {
                ImGui::TextWrapped("%u x %u / DXGI %u", r.width, r.height, r.format);
            }
            else
            {
                ImGui::TextDisabled("Missing");
            }
            if (previewTexture && previewSelected && previewWidth && previewHeight &&
                r.present)
            {
                float w = (std::max)(1.f, ImGui::GetContentRegionAvail().x);
                float h =
                    (std::min)(180 * scale, w * float(previewHeight) / float(previewWidth / 3));
                w = h * float(previewWidth / 3) / float(previewHeight);
                const float tileWidth = float(previewWidth / 3), atlasWidth = float(previewWidth);
                ImGui::Image(
                    previewTexture, ImVec2(w, h),
                    ImVec2((n * tileWidth + .5f) / atlasWidth, .5f / previewHeight),
                    ImVec2(((n + 1) * tileWidth - .5f) / atlasWidth, 1 - .5f / previewHeight));
            }
            else
            {
                ImGui::TextDisabled("Preview unavailable");
            }
            if (n == 0)
            {
                ItemTip("Display-range color preview.");
            }
            if (n == 1)
            {
                bool flipped = nr::DepthFlip();
                if (ImGui::Checkbox("Flip depth vertically", &flipped))
                {
                    nr::SetDepthFlip(flipped);
                }
                ItemTip(
                    "Flip depth for previews, motion estimation and NR guides. Enable if the game's depth is upside down. Default: off.");
                bool contrast = nr::DepthContrast();
                if (ImGui::Checkbox("Depth contrast", &contrast))
                {
                    nr::SetDepthContrast(contrast);
                }
                ItemTip(
                    "Increase depth-preview contrast to make near/far differences easier to see. Does not change model depth. Default: off.");
                // Escape hatch: keep the depth detection selected, ignoring later re-selection.
                bool pinned = nr::depth::Pinned();
                if (ImGui::Checkbox("Lock current depth", &pinned))
                {
                    if (pinned)
                    {
                        nr::depth::PinCurrent();
                    }
                    else
                    {
                        nr::depth::ClearPin();
                    }
                }
                ItemTip(
                    "Keep the depth the scan last selected instead of re-selecting every frame. Use when detection picks a pass depth. Not saved; cleared on device reset. Default: off.");
            }
            if (n == 2)
            {
                float value = nr::DisplayScale();
                ImGui::SetNextItemWidth(
                    (std::max)(80 * scale, ImGui::GetContentRegionAvail().x - 70 * scale));
                bool finished = false;
                if (PanelSliderFloat("Scale", &value, .001f, 64.f, "%.3g",
                                     ImGuiSliderFlags_Logarithmic, &finished))
                {
                    nr::SetDisplayScale(value, false);
                    scaleEdited = true;
                }
                if (finished && scaleEdited)
                {
                    nr::SetDisplayScale(value, true);
                    scaleEdited = false;
                }
                if (ImGui::SmallButton("Reset scale"))
                {
                    nr::SetDisplayScale(1);
                }
                ItemTip("Restore motion-vector preview scale to 1.");
                ImGui::TextWrapped("MV scale: %.4g, %.4g", resources.guides.motionScaleX,
                                   resources.guides.motionScaleY);
                ItemTip("Red +X, green +Y, gray zero. Scale changes preview only.");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
        ImGui::Separator();
        // The estimator only runs when the caller supplies no motion vectors, so its tuning does
        // nothing while the game's own DLSS path hands us native MV.
        const bool estimatorInUse =
            pass.generatedMotion || pass.inputOrigin == nr::InputOrigin::NativeCapture;
        if (device11 && !estimatorInUse)
        {
            ImGui::TextDisabled("MV comes from the game; estimator settings are hidden.");
            ItemTip(
                "The D3D11 estimator and its FOV / far plane / depth mapping only run when the game provides no motion vectors. With DLSS active the game's own MV is used as is.");
        }
        if (device11 && estimatorInUse)
        {
            {
                auto options = nr::FlowOptions();
                auto slider = [&](unsigned n, const char* label, float value, float low,
                                  float high, float initial, const char* tip)
                {
                    float edited = flowPending[n] ? flowEditing[n] : value;
                    bool done = false;
                    SliderResetWidth(label);
                    if (PanelSliderFloat(label, &edited, low, high, "%.4g",
                                         n ? ImGuiSliderFlags_Logarithmic : 0, &done))
                    {
                        flowPending[n] = true;
                        flowEditing[n] = edited;
                    }
                    ItemTip(tip);
                    ImGui::SameLine();
                    ImGui::PushID(label);
                    if (ResetValue(value, initial))
                    {
                        flowEditing[n] = value;
                        flowPending[n] = true;
                        done = true;
                    }
                    ImGui::PopID();
                    if (done && flowPending[n])
                    {
                        CommitFlowOptions();
                    }
                };
                slider(
                    0, "MV FOV", options.fov, 1, 179, 60, "Vertical FOV for depth distance and MV filtering. Default: 60 degrees; the game camera is not read automatically. Applied after editing; does not modify NR depth.");
                slider(
                    1, "MV far plane", options.farPlane, 2, 100000, 1000,
                    "Depth linearization far plane. Default: 1000. Match the game's depth convention. Applied after editing; Ctrl+click or right-click to type.");
                slider(
                    2, "MV depth multiplier", options.depthMultiplier, .001f, 1000, 1,
                    "Scale raw device depth before linearization. Default: 1. Applied after editing; does not modify NR depth.");
                auto mapping = [&](const char* label, float& x, float& y, float initial, const char* tip)
                {
                    float value[2]{x, y};
                    bool changed = ImGui::InputFloat2(label, value, "%.5g");
                    ItemTip(tip);
                    ImGui::SameLine();
                    ImGui::PushID(label);
                    if (ImGui::SmallButton("Reset"))
                    {
                        value[0] = value[1] = initial;
                        changed = true;
                    }
                    ImGui::PopID();
                    if (changed)
                    {
                        x = value[0];
                        y = value[1];
                        nr::SetFlowOptions(options);
                        options = nr::FlowOptions();
                    }
                };
                mapping("MV depth XY scale", options.depthScaleX, options.depthScaleY, 1, "LAUNCHPAD depth input scale: UV is divided by XY. Defaults: 1, 1. Only changes the estimator.");
                mapping("MV depth XY offset", options.depthOffsetX, options.depthOffsetY, 0, "LAUNCHPAD normalized offsets: X subtracts half the value; Y adds half. Defaults: 0, 0.");
                mapping("MV depth pixel offset", options.depthPixelOffsetX, options.depthPixelOffsetY, 0, "Nonzero pixel offset overrides normalized offset on that axis, as in LAUNCHPAD. Defaults: 0, 0.");
                if (ImGui::Checkbox("Logarithmic depth", &options.logarithmic))
                {
                    nr::SetFlowOptions(options);
                }
                ItemTip(
                    "Decode logarithmic device depth before MV projection. Default: off. Use only when the game encodes logarithmic depth.");
            }
        }
    }
}
// Short names for the depth formats a capture can land on; anything else prints its number.
const char* DepthFormatName(unsigned value, char* text, size_t size)
{
    switch (static_cast<DXGI_FORMAT>(value))
    {
        case DXGI_FORMAT_R32G8X24_TYPELESS: // DSV resource format of D32S8X24
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            return "D32S8X24";
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_TYPELESS:
            return "D32";
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
            return "D24S8";
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_R16_TYPELESS:
            return "D16";
        default:
            break;
    }
    std::snprintf(text, size, "fmt=%u", value);
    return text;
}

void Panel::Impl::BuildPipelineStatus(const nr::ModelSettings& settings, const nr::PassStatus& pass,
                                      const nr::ModelStatus& model)
{
    if (!ImGui::CollapsingHeader("Current pipeline", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }
    const ImVec4 green(.35f, .9f, .55f, 1), amber(1, .78f, .3f, 1), muted(.7f, .7f, .7f, 1);
    const auto now = GetTickCount64();
    const auto dlss = nr::GameDlssSnapshot();
    const bool recentDlss = dlss.observed && now - dlss.lastEvaluation <= 2000;
    const bool activeDlss = recentDlss && !dlss.released && dlss.result == 1;
    const char* dlssText = !dlss.observed     ? "Not observed"
                           : dlss.released    ? "Last observed SR feature released"
                           : !recentDlss      ? "No recent SR evaluation"
                           : dlss.result != 1 ? "Last SR evaluation failed"
                                              : "Active";
    ImGui::TextColored(activeDlss ? green : amber, "Game DLSS / DLAA: %s", dlssText);
    ItemTip(
        "Based on actual game NGX SR evaluation, including while NR is off. No recent evaluation can mean DLSS is off, a menu/pause, or an unobserved route; it is not proof that the game setting is off.");
    const bool currentInput = pass.hasInput && now - pass.inputTime <= 2000;
    const bool running = settings.enabled && currentInput && pass.ready && pass.actualPasses;
    // The pass owns the wait-reason log. A second writer here alternated with it every
    // frame and produced thousands of "NR waiting:" lines per minute.
    const bool failed =
        FAILED(model.error) || pass.failedPass || (FAILED(pass.error) && !pass.frameSkipped);
    ImGui::TextColored(!settings.enabled ? muted
                       : running         ? green
                                         : amber,
                       "NR: %s",
                       !settings.enabled ? "Off"
                       : failed          ? "Failed; toggle NR off/on to retry"
                       : !currentInput   ? "Waiting for current input"
                       : running         ? "Processing"
                                         : "Original frame / waiting");
    if (settings.enabled && !failed)
    {
        if (model.state != State::Ready)
        {
            ImGui::TextWrapped("%s", Utf8(model.reason).c_str());
        }
        else
        {
            ImGui::TextWrapped("%s", pass.reason);
        }
    }
    if (!currentInput)
    {
        ImGui::TextDisabled("NR inputs: no recent input received");
        return;
    }
    const char* origin =
        pass.inputOrigin == nr::InputOrigin::GameDlss        ? "game post-DLSS output"
        : pass.inputOrigin == nr::InputOrigin::NativeCapture ? "D3D11 scene capture"
                                                             : "unidentified source";
    ImGui::TextWrapped("Last NR color: %s%s", origin,
                       pass.sceneLinear ? " + white normalization / encoding"
                                        : " (display domain)");
    ImGui::TextWrapped("Depth: %s%s%s",
                       pass.inputOrigin == nr::InputOrigin::GameDlss ? "game native"
                                                                     : "captured / supplied",
                       pass.inputConverted[1] ? " + format conversion" : "",
                       pass.depthFlipped ? " + vertical flip" : "");
    // Only the D3D11 capture path ranks candidates; a game-supplied depth has no winner and
    // the snapshot needs the global lock, so it is not queried at all.
    if (pass.inputOrigin != nr::InputOrigin::GameDlss)
    {
        const auto captured = nr::DepthSnapshot();
        if (captured.frameWidth)
        {
            if (!captured.present)
            {
                ImGui::TextDisabled("  no depth snapshot this frame (frame %ux%u)",
                                    captured.frameWidth, captured.frameHeight);
            }
            else
            {
                const double factor = captured.frameWidth
                                          ? double(captured.width) / double(captured.frameWidth)
                                          : 0.0;
                char formatText[24]{};
                ImGui::TextDisabled(
                    "  %ux%u %s / frame %ux%u (%.2fx)%s%s | %u candidates%s",
                    captured.width, captured.height,
                    DepthFormatName(captured.format, formatText, sizeof(formatText)),
                    captured.frameWidth, captured.frameHeight, factor,
                    captured.inverted ? " + inverted" : "",
                    captured.deferred ? " (deferred)" : "", captured.candidates,
                    captured.written ? "" : " | not bound with writes this frame");
            }
        }
    }
    ImGui::PushStyleColor(ImGuiCol_Text, pass.generatedMotion ? amber : green);
    ImGui::TextWrapped("MV: %s%s | scale %.5g, %.5g",
                       pass.generatedMotion                            ? "plugin generated"
                       : pass.inputOrigin == nr::InputOrigin::GameDlss ? "game native"
                                                                       : "supplied",
                       pass.inputConverted[2] ? " + format conversion" : "",
                       double(pass.motionScaleX), double(pass.motionScaleY));
    ImGui::PopStyleColor();
    ItemTip(
        "Origin and required adapters are taken from the NR input path, not from the Resources preview. Copying/sharing unchanged data does not mean MV was generated. Model scale values are shown separately.");
}

void Panel::Impl::BuildUi()
{
    bool resourcesActive = false;
    const auto display = ImGui::GetIO().DisplaySize;
    const float scale = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
    if (display.x != layoutDisplay.x || display.y != layoutDisplay.y || scale != layoutScale)
    {
        repairPosition = true;
    }
    layoutDisplay = display;
    layoutScale = scale;
    ImGui::SetNextWindowSize(ImVec2((std::min)(540 * scale, (std::max)(300.f, display.x - 56)),
                                    (std::min)(560 * scale, (std::max)(240.f, display.y - 56))),
                             ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(28, 28), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(200, 180),
        ImVec2((std::max)(200.f, display.x - 56), (std::max)(180.f, display.y - 56)));
    // Keep the existing ImGui ID while updating the visible product name.
    if (ImGui::Begin("DLSSNR_LiteDX11###Lite", &status.visible, ImGuiWindowFlags_NoCollapse))
    {
        // Repair after viewport/DPI changes, never fight a user's active drag.
        if (repairPosition && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            const auto size = ImGui::GetWindowSize();
            const auto pos = ImGui::GetWindowPos();
            const ImVec2 bounded((std::clamp)(pos.x, 0.f, (std::max)(0.f, display.x - size.x)),
                                 (std::clamp)(pos.y, 0.f, (std::max)(0.f, display.y - size.y)));
            if (bounded.x != pos.x || bounded.y != pos.y)
            {
                ImGui::SetWindowPos(bounded);
            }
            repairPosition = false;
        }
        if (ImGui::BeginTabBar("LitePages"))
        {
            if (ImGui::BeginTabItem("Overview"))
            {
                ImGui::BeginChild("Overview contents", ImVec2(0, 0));
                ImGui::TextColored(ImVec4(1, .55f, .2f, 1), "DLSSNR_LiteDX11");
                ImGui::SameLine();
                ImGui::TextDisabled("%s", LITE_VERSION);
                ImGui::Separator();
                ImGui::TextWrapped("DirectX 11 | %u x %u", status.width, status.height);
                ImGui::Spacing();
                ImGui::Separator();
                auto modelSettings = nr::GetModelSettings();
                bool modelChanged = false;
                modelChanged |= ImGui::Checkbox("Neural rendering", &modelSettings.enabled);
                ItemTip(
                    "Enable NR processing. Requires a supported NVIDIA device and the NR model. Default: off.");
                const auto model = nr::Runtime().Snapshot();
                const auto pass = nr::LastPass();
                BuildPipelineStatus(modelSettings, pass, model);
                ImGui::Separator();
                if (ImGui::CollapsingHeader("Model parameters",
                                            ImGuiTreeNodeFlags_DefaultOpen))
                {
                    int preset = int(modelSettings.pass.preset),
                        style = int(modelSettings.pass.style);
                    if (ImGui::Combo("Model preset", &preset,
                                     "Default (model choice)\0A\0B\0"))
                    {
                        modelSettings.pass.preset = unsigned(preset);
                        modelChanged = true;
                    }
                    ItemTip(
                        "A: original model. B: optimized model (requires 310.9.16.0 or newer). Default lets the DLL choose: B in 310.9.16.0; the older DLL falls back to its original model. Changing preset rebuilds NR histories.");
                    if (ImGui::Combo("Style", &style,
                                     "Default (standard)\0Natural\0Cinematic\0"))
                    {
                        modelSettings.pass.style = unsigned(style);
                        modelChanged = true;
                    }
                    ItemTip(
                        "Choose the model's visual style. Changing it rebuilds NR histories. Default: standard.");
                    auto slider = [&](const char* label, float& value, float low, float high,
                                      float initial)
                    {
                        SliderResetWidth(label);
                        modelChanged |= PanelSliderFloat(label, &value, low, high, "%.3f");
                        if (std::strcmp(label, "Difference gain") == 0)
                        {
                            ItemTip("1: normal NR | 0: original | 2: doubles the change. Applied once after NR.");
                        }
                        ImGui::SameLine();
                        ImGui::PushID(label);
                        modelChanged |= ResetValue(value, initial);
                        ImGui::PopID();
                    };
                    auto tuningSlider = [&](unsigned n, const char* label, float& value,
                                            float low, float initial)
                    {
                        float editing = tuningPending[n] ? tuningValues[n] : value;
                        bool finished = false;
                        SliderResetWidth(label);
                        if (PanelSliderFloat(label, &editing, low, 2, "%.3f", 0, &finished))
                        {
                            tuningValues[n] = editing;
                            tuningPending[n] = true;
                        }
                        if (finished && tuningPending[n])
                        {
                            value = tuningValues[n];
                            tuningPending[n] = false;
                            modelChanged = true;
                        }
                        ImGui::SameLine();
                        ImGui::PushID(label);
                        if (ResetValue(value, initial))
                        {
                            tuningPending[n] = false;
                            modelChanged = true;
                        }
                        ImGui::PopID();
                    };
                    tuningSlider(0, "Intensity", modelSettings.pass.intensity, 0, 1);
                    tuningSlider(1, "Local structure", modelSettings.pass.structure, 0, 1);
                    tuningSlider(2, "Local tone", modelSettings.pass.tone, 0, 1);
                    tuningSlider(3, "Skin structure", modelSettings.pass.skin, -1, -1);
                    bool autoMask = modelSettings.pass.autoMask != 0;
                    if (ImGui::Checkbox("AutoMask", &autoMask))
                    {
                        modelSettings.pass.autoMask = autoMask ? 1u : 0u;
                        modelChanged = true;
                    }
                    ItemTip(
                        "Let NR derive its own application mask. Changing this rebuilds histories. Default: on.");
                    bool uiCorrection = modelSettings.pass.uiCorrection != 0;
                    if (ImGui::Checkbox("UI correction", &uiCorrection))
                    {
                        modelSettings.pass.uiCorrection = uiCorrection ? 1u : 0u;
                        modelChanged = true;
                    }
                    ItemTip(
                        "Pass the model's UI correction hint (DLSSNR.UICorrection). DLSSNR_LiteDX11 still captures after upscale, before or after the game's UI depending on the game. Changing this rebuilds histories. Default: off.");
                    slider("Difference gain", modelSettings.pass.differenceGain, 0, 2, 1);
                    modelChanged |= ImGui::Checkbox("Preserve HDR highlights", &modelSettings.pass.composed);
                    ItemTip("Compose the model result with the original input; off applies the model output directly. Switching resets NR history.");
                }
                else
                {
                    modelChanged |= CommitTuning(&modelSettings);
                }
                if (modelChanged)
                {
                    nr::SetModelSettings(modelSettings);
                }
                if (ImGui::CollapsingHeader("GPU timing / errors"))
                {
                    if (pass.timingValid)
                    {
                        ImGui::TextWrapped("Completed NR GPU: %.3f ms (model %.3f ms)",
                                           double(pass.gpuMilliseconds),
                                           double(pass.modelMilliseconds));
                        ItemTip(
                            "Timing runs while the panel is open; includes encode, guides, white meter, model and resolve. Excludes feature creation, estimated MV and DX11 bridge.");
                    }
                    if (FAILED(model.error))
                    {
                        ImGui::Text("Runtime HRESULT 0x%08lX | NGX 0x%08X",
                                    static_cast<unsigned long>(model.error), model.ngxResult);
                    }
                    ImGui::TextDisabled("Diagnostics level: %u (0 off, 1 basic, 2 verbose)",
                                        unsigned(DetailLevel()));
                    ItemTip(
                        "Diagnostics.Level is read at startup. The log plumbing exists but no code path emits a line yet, so the level has no visible effect.");
                }
                const auto config = ConfigStorageSnapshot();
                if (config.error || config.fallbackCause)
                {
                    if (config.error)
                    {
                        ImGui::TextWrapped("Configuration save failed (Windows error %lu).",
                                           config.error);
                    }
                    else
                    {
                        ImGui::TextWrapped("Configuration fallback (Windows error %lu).",
                                           config.fallbackCause);
                    }
                    ItemTip(
                        "Saves run in the background. If the game directory blocks writing, a per-game profile is used. Failed saves leave parameters active in memory.");
                }
                ImGui::Spacing();
                ImGui::TextDisabled("Insert: show / hide panel");
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            else
            {
                CommitTuning();
            }
            if (ImGui::BeginTabItem("White point"))
            {
                ImGui::BeginChild("White point contents", ImVec2(0, 0));
                auto modelSettings = nr::GetModelSettings();
                bool modelChanged = false;
                const auto pass = nr::LastPass();
                if (!modelSettings.enabled)
                {
                    ImGui::TextDisabled(
                        "NR is off; measurements wait until NR processing is active.");
                }
                int whiteMode = int(modelSettings.pass.white.mode);
                if (ImGui::Combo("White point", &whiteMode,
                                 "Game exposure\0Measured input\0Manual\0"))
                {
                    modelSettings.pass.white.mode = nr::WhiteMode(whiteMode);
                    modelChanged = true;
                }
                ItemTip(
                    "Game exposure uses the game's exposure inputs. Measured input estimates white from the original image and applies the filter below. Manual uses a fixed value. Display input bypasses normalization. Default: Game exposure.");
                const nr::WhiteSettings defaults;
                auto whiteSlider = [&](const char* label, float& value, float low, float high,
                                       float initial, const char* format = "%.3f",
                                       ImGuiSliderFlags flags = 0)
                {
                    SliderResetWidth(label);
                    modelChanged |= PanelSliderFloat(label, &value, low, high, format, flags);
                    ImGui::SameLine();
                    ImGui::PushID(label);
                    modelChanged |= ResetValue(value, initial);
                    ImGui::PopID();
                };
                if (whiteMode == 0)
                {
                    int ratio = modelSettings.pass.white.reverseRatio ? 1 : 0;
                    if (ImGui::Combo("Exposure ratio", &ratio,
                                     "Pre-exposure / exposure\0Exposure / pre-exposure\0"))
                    {
                        modelSettings.pass.white.reverseRatio = ratio != 0;
                        modelChanged = true;
                    }
                    ItemTip(
                        "Choose which side of the game's exposure pair the white divisor takes. Both directions are evaluated live on the GPU from the exposure texture; the texture value is never inverted, only its position in the fraction.");
                    modelChanged |= ImGui::Checkbox("Use pre-exposure",
                                                    &modelSettings.pass.white.usePreExposure);
                    ItemTip("Use DLSS.Pre.Exposure. Unchecked substitutes 1. Default: on.");
                    modelChanged |= ImGui::Checkbox("Use exposure scale",
                                                    &modelSettings.pass.white.useExposureScale);
                    ItemTip("Use DLSS.Exposure.Scale on the scalar path. Unchecked substitutes 1. A supplied, enabled exposure texture keeps its existing priority.");
                    modelChanged |= ImGui::Checkbox("Read game's exposure texture",
                                                    &modelSettings.pass.white.useTexture);
                    ItemTip(
                        "Use a supplied 1 x 1 game exposure texture. Off selects the scalar path; unchecked scalar inputs are 1. For texture only, leave this checked and uncheck both scalar inputs. Default: on.");
                    whiteSlider("Game exponent", modelSettings.pass.white.exponent, .1f, 4.f,
                                defaults.exponent, "%.3f");
                    ItemTip(
                        "Raises the game exposure ratio to this power before the trim. 1 is the plain linear mapping. Below 1 compresses the range so one trim covers both bright and dark scenes; above 1 expands it. Only affects the game exposure white point.");
                }
                whiteSlider("Manual / fallback", modelSettings.pass.white.manual, 1e-8f, 1024.f,
                            defaults.manual, "%.9g", ImGuiSliderFlags_Logarithmic);
                whiteSlider("White trim", modelSettings.pass.white.trim, .001f, 1024.f,
                            defaults.trim, "%.6g", ImGuiSliderFlags_Logarithmic);
                ImGui::SeparatorText("Current white point");
                const ImVec4 green(.35f, .9f, .55f, 1), blue(.45f, .75f, 1, 1),
                    amber(1, .78f, .3f, 1), muted(.7f, .7f, .7f, 1);
                const bool liveWhite = pass.white.source == nr::WhiteSource::Game &&
                                       pass.gameExposureTexture &&
                                       modelSettings.pass.white.useTexture;
                if (!modelSettings.enabled || !pass.hasInput)
                {
                    ImGui::TextColored(muted, "Current white: -- (%s)",
                                       modelSettings.enabled ? "waiting for input" : "NR off");
                }
                else if (!pass.sceneLinear)
                {
                    ImGui::TextColored(blue, "Current white: 1 (display input bypass)");
                }
                else if (liveWhite)
                {
                    ImGui::TextColored(green, "Current white: live game exposure (GPU)%s",
                                       pass.gpuDivisorValid ? "" : " / awaiting sample");
                    if (pass.gpuDivisorValid)
                    {
                        ImGui::TextColored(blue, "Last completed white: %.6g",
                                           double(pass.gpuDivisor));
                    }
                }
                else
                {
                    const bool fallback = !pass.white.valid ||
                                          pass.white.source == nr::WhiteSource::Waiting ||
                                          pass.white.source == nr::WhiteSource::Held;
                    const bool manual = pass.white.source == nr::WhiteSource::Manual;
                    ImGui::TextColored(fallback ? amber
                                       : manual ? blue
                                                : green,
                                       "Current white: %.6g (%s)", double(pass.white.value),
                                       fallback ? "fallback / held"
                                       : manual ? "manual"
                                       : pass.white.source == nr::WhiteSource::Game
                                           ? "game exposure"
                                           : "measured input");
                }
                ItemTip(
                    "GPU-live exposure reads the texture on the GPU; the sample is the last completed readback. Display input uses divisor 1. Fallback uses manual white, or holds the last valid value.");
                ImGui::SeparatorText("White point status");
                ImGui::TextWrapped("Input: %s",
                                   !pass.hasInput ? "Not received"
                                   : pass.sceneLinear
                                       ? "Scene linear; white normalization and encoding"
                                       : "Display referred; white normalization bypassed");
                if (pass.gpuExposureValid)
                {
                    ImGui::Text("Last sampled game exposure: %.5g", double(pass.gpuExposure));
                }
                else
                {
                    ImGui::TextDisabled(
                        "Game exposure texture value: unavailable / not measured");
                }
                if (pass.preExposurePresent)
                {
                    ImGui::Text("Game pre-exposure: %.5g", double(pass.preExposure));
                }
                else
                {
                    ImGui::TextDisabled("Game pre-exposure: not supplied");
                }
                if (pass.exposureScalePresent)
                {
                    ImGui::Text("Game exposure scale: %.5g", double(pass.exposureScale));
                }
                else
                {
                    ImGui::TextDisabled("Game exposure scale: not supplied");
                }
                ImGui::SeparatorText("Original input statistics");
                if (FAILED(pass.whiteMeasurementError))
                {
                    ImGui::TextWrapped(
                        "White measurement failed (0x%08lX); manual/history fallback.",
                        static_cast<unsigned long>(pass.whiteMeasurementError));
                }
                if (pass.white.statisticsValid)
                {
                    ImGui::Text("Mean %.5g | log-average %.5g", double(pass.white.mean),
                                double(pass.white.logAverage));
                    ImGui::Text("p50 %.5g | p90 %.5g", double(pass.white.p50),
                                double(pass.white.p90));
                    ImGui::Text("p99 %.5g | peak %.5g", double(pass.white.p99),
                                double(pass.white.peak));
                }
                if (ImGui::CollapsingHeader("White point filter",
                                            ImGuiTreeNodeFlags_DefaultOpen))
                {
                    // These five only drive the Measured branch of WhitePoint::Resolve.
                    if (whiteMode == 1)
                    {
                        whiteSlider("Mid-gray anchor", modelSettings.pass.white.anchor, .02f, 1.f,
                                    defaults.anchor);
                        whiteSlider("Filter seconds", modelSettings.pass.white.tau, .001f, 60.f,
                                    defaults.tau, "%.3g", ImGuiSliderFlags_Logarithmic);
                        whiteSlider("Deadband", modelSettings.pass.white.deadband, 0, 1.f,
                                    defaults.deadband);
                        whiteSlider("Attack (stops/s)", modelSettings.pass.white.attack, 0, 16.f,
                                    defaults.attack, "%.2f");
                        whiteSlider("Release (stops/s)", modelSettings.pass.white.release, 0, 16.f,
                                    defaults.release, "%.2f");
                    }
                    SliderResetWidth("Measure every N frames");
                    int every = int(modelSettings.pass.white.every);
                    if (PanelSliderInt("Measure every N frames", &every, 1, 120))
                    {
                        modelSettings.pass.white.every = unsigned(every);
                        modelChanged = true;
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Reset##WhiteEvery"))
                    {
                        modelSettings.pass.white.every = defaults.every;
                        modelChanged = true;
                    }
                    ItemTip("Restore the measurement interval to 4 processed NR frames.");
                }
                if (modelChanged)
                {
                    nr::SetModelSettings(modelSettings);
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Resources"))
            {
                resourcesActive = true;
                if (!nr::PreviewSelected())
                {
                    nr::SetPreviewSelected(true);
                }
                if (ImGui::BeginChild("Resource contents", ImVec2(0, 0)))
                {
                    BuildResourceUi(scale);
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            else
            {
                CommitFlowOptions();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
    // A visible panel on another page has no preview demand of its own.
    if (status.visible && !resourcesActive && nr::PreviewSelected())
    {
        nr::SetPreviewSelected(false);
    }
    SyncPreviewDemand();
    if (!status.visible)
    {
        CapturePanelInput(hwnd, false);
    }
}

}

// SPDX-License-Identifier: GPL-3.0-or-later
// Panel::Impl: the panel's state. Its methods are split across three files —
// panel.cpp (construction, window subclass and teardown), panel_gpu.cpp (the
// Direct3D 11 renderer) and panel_ui.cpp (the ImGui window). Nothing outside
// panel*.cpp sees this type.
#pragma once
#include "panel.hpp"
#include "../graphics.hpp"
#include "window_subclass.hpp"
#include "../nr/capture11.hpp"
#include "../nr/model_runtime.hpp"
#include "../nr/model_settings.hpp"
#include <imgui.h>
#include <wrl/client.h>
#include <array>
#include <memory>
#include <vector>

namespace lite
{
using Microsoft::WRL::ComPtr;
// Shared by the shell and the renderer; holds no state of its own.
namespace panel_detail
{
inline void Check(HRESULT hr)
{
    if (FAILED(hr))
    {
        throw hr;
    }
}
// The render-target view format for a swapchain buffer: an sRGB buffer is drawn
// through its UNORM sibling (the interface's colours are already sRGB).
inline DXGI_FORMAT RtvFormat(DXGI_FORMAT format)
{
    if (format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || format == DXGI_FORMAT_R8G8B8A8_TYPELESS)
    {
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    if (format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || format == DXGI_FORMAT_B8G8R8A8_TYPELESS)
    {
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    }
    return format;
}
}
struct Panel::Impl
{
    IDXGISwapChain* swap{}; // Borrowed wrapper; the wrapper owns the native swapchain.
    HWND hwnd{};
    bool windowAttached{};
    ImGuiContext* context{};
    GraphicsStatus status;
    bool win32Ready{}, rendererReady{}, inputFocused{}, ignoreMenuRelease{};
    bool previewDemand{}, scaleEdited{};
    std::array<float, 4> tuningValues{};
    std::array<bool, 4> tuningPending{};
    std::array<float, 3> flowEditing{};
    std::array<bool, 3> flowPending{};
    ComPtr<ID3D11Device> device11;
    ComPtr<ID3D11DeviceContext> immediate;
    // NativeCapture is fixed per game by the INI before the panel exists; never re-evaluated.
    bool capturing{};
    float dpiScale = 1;
    float fontDpiScale = 1;
    ImGuiStyle baseStyle;
    ImVec2 layoutDisplay{};
    float layoutScale{};
    bool repairPosition = true;
    DXGI_FORMAT format{};
    ComPtr<ID3D11ShaderResourceView> previewImage11;
    // Built in InitRenderer, released by DestroyRenderer so a swapchain replacement can reach its
    // final Release; the same lifetime as the ImGui device objects.
    ComPtr<ID3D11RenderTargetView> panelRtv;
    ComPtr<ID3D11ShaderResourceView> previewSizedView; // preview view whose extents are cached
    ImTextureID previewTexture{};
    bool previewSelected{};
    UINT previewWidth{}, previewHeight{};
    bool previewDisplayed{};
    // Where a failure happened; printed by Failure().
    const wchar_t* renderStage = L"panel input/window callback";
    // Committed on edit end, never per frame of a drag.
    void CommitFlowOptions();
    bool CommitTuning(nr::ModelSettings* target = nullptr);
    void SyncPreviewDemand();
    ~Impl();
    static WindowSubclassRegistry<Impl>& Windows()
    {
        static auto* windows = new WindowSubclassRegistry<Impl>;
        return *windows;
    }
    static bool RouteKeyboard(MSG& message, bool removed);
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    void Unsubclass();
    void DestroyRenderer();
    void InitRenderer();
    void BuildResourceUi(float scale);
    void BuildPipelineStatus(const nr::ModelSettings& settings, const nr::PassStatus& pass,
                             const nr::ModelStatus& model);
    void BuildUi();
    void Draw11();
    void Failure(HRESULT hr);
};
}

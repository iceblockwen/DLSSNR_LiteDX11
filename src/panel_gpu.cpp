// SPDX-License-Identifier: GPL-3.0-or-later
// The panel's Direct3D 11 renderer: device objects, the ImGui draw, and failure.
#include "panel_impl.hpp"
#include "input.hpp"
#include "nr/depth11.hpp"
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

namespace lite {
using panel_detail::Check;
using panel_detail::RtvFormat;

void Panel::Impl::DestroyRenderer() {
    if (context)
        ImGui::SetCurrentContext(context);
    if (rendererReady) {
        ImGui_ImplDX11_Shutdown();
        rendererReady = false;
    }
    // A retained view keeps its resource alive; drop both before the engine replaces them.
    panelRtv.Reset();
    previewSizedView.Reset();
}

void Panel::Impl::InitRenderer() {
    DXGI_SWAP_CHAIN_DESC desc{};
    Check(swap->GetDesc(&desc));
    status.width = desc.BufferDesc.Width;
    status.height = desc.BufferDesc.Height;
    format = RtvFormat(desc.BufferDesc.Format);
    ComPtr<ID3D11Texture2D> buffer;
    Check(swap->GetBuffer(0, IID_PPV_ARGS(&buffer)));
    D3D11_TEXTURE2D_DESC texture{};
    buffer->GetDesc(&texture);
    status.width = texture.Width;
    status.height = texture.Height;
    if (!ImGui_ImplDX11_Init(device11.Get(), immediate.Get()))
        throw E_FAIL;
    rendererReady = true;
    // Built once per renderer generation: BeforeResize calls DestroyRenderer first, so a buffer
    // replacement always re-enters here instead of leaving a stale view behind.
    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
    rtvDesc.Format = RtvFormat(texture.Format);
    rtvDesc.ViewDimension = texture.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS
                                                         : D3D11_RTV_DIMENSION_TEXTURE2D;
    Check(device11->CreateRenderTargetView(buffer.Get(), &rtvDesc, &panelRtv));
    if (!ImGui_ImplDX11_CreateDeviceObjects())
        throw E_FAIL;
    status.panel = State::Ready;
    status.error = S_OK;
    status.reason[0] = 0;
}

void Panel::Impl::Draw11() {
    nr::depth::Internal internal;
    previewSelected = nr::PreviewSelected();
    previewTexture = 0;
    previewImage11 = nr::Image11(device11.Get());
    if (previewImage11) {
        previewTexture = reinterpret_cast<ImTextureID>(previewImage11.Get());
        if (previewImage11.Get() != previewSizedView.Get()) {
            ComPtr<ID3D11Resource> r;
            previewImage11->GetResource(&r);
            ComPtr<ID3D11Texture2D> t;
            if (SUCCEEDED(r.As(&t))) {
                D3D11_TEXTURE2D_DESC desc{};
                t->GetDesc(&desc);
                previewWidth = desc.Width;
                previewHeight = desc.Height;
            }
            previewSizedView = previewImage11;
        }
    }
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    UpdatePanelPointer(hwnd);
    ImGui::NewFrame();
    BuildUi();
    ImGui::Render();
    ID3D11RenderTargetView *targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView *depth{};
    ComPtr<ID3D11HullShader> hull;
    ComPtr<ID3D11DomainShader> domain;
    ComPtr<ID3D11ComputeShader> compute;
    std::array<ID3D11ClassInstance *, 256> hullClasses{}, domainClasses{}, computeClasses{};
    UINT hullCount = 256, domainCount = 256, computeCount = 256;
    immediate->HSGetShader(&hull, hullClasses.data(), &hullCount);
    immediate->DSGetShader(&domain, domainClasses.data(), &domainCount);
    immediate->CSGetShader(&compute, computeClasses.data(), &computeCount);
    immediate->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, &depth);
    auto *view = panelRtv.Get();
    immediate->OMSetRenderTargets(1, &view, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    if (previewDisplayed)
        nr::PreviewResult(S_OK);
    immediate->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, depth);
    // ImGui 1.90.9 clears these stages but does not restore them itself.
    immediate->HSSetShader(hull.Get(), hullClasses.data(), hullCount);
    immediate->DSSetShader(domain.Get(), domainClasses.data(), domainCount);
    immediate->CSSetShader(compute.Get(), computeClasses.data(), computeCount);
    for (auto *instance : hullClasses)
        if (instance)
            instance->Release();
    for (auto *instance : domainClasses)
        if (instance)
            instance->Release();
    for (auto *instance : computeClasses)
        if (instance)
            instance->Release();
    for (auto *target : targets)
        if (target)
            target->Release();
    if (depth)
        depth->Release();
}

void Panel::Impl::Failure(HRESULT hr) {
    CapturePanelInput(hwnd, false);
    status.error = hr;
    status.panel = State::Failed;
    status.visible = false;
    SyncPreviewDemand();
    wcscpy_s(status.reason, L"Overlay failed; original DXGI presentation continues");
    wchar_t detail[256]{};
    const HRESULT removed=device11->GetDeviceRemovedReason();
    swprintf_s(detail,L"Overlay failure: stage=%s HRESULT=0x%08X deviceRemoved=0x%08X",renderStage,unsigned(hr),unsigned(removed));
    Log(detail);
}

}

#pragma once
#include "frame.hpp"
#include "dense_flow.hpp"
#include "../runtime.hpp"
namespace lite::nr
{
struct GameDlssStatus
{
    bool observed{}, released{};
    unsigned result{};
    UINT64 lastEvaluation{};
};
GameDlssStatus GameDlssSnapshot();
struct ResourceInfo
{
    unsigned width{}, height{}, format{};
    bool present{};
};
// Last captured depth winner, published for the panel. Cleared when a frame had no snapshot.
struct DepthStatus
{
    bool present{};
    unsigned width{}, height{}, format{}; // winner source texture
    unsigned frameWidth{}, frameHeight{}; // frame the winner was ranked against
    unsigned candidates{};
    bool inverted{}, deferred{}, written{};
};
// Verbose only: GPU milliseconds of the last completed bridge recording, 0 when not measured.
float BridgeChainMilliseconds();
DepthStatus DepthSnapshot();
struct PreviewStatus
{
    State state = State::Waiting;
    HRESULT error = S_OK;
    ResourceInfo resources[3]{};
    Guides guides;
    HRESULT previewError = S_OK;
    wchar_t reason[192]{L"Waiting for frame resources"};
};
bool PreviewSelected();
// Debug demand only; model resource processing must use its own demand.
bool PreviewActive();
bool PanelVisible();
void SetPanelVisible(const void* owner, bool visible);
void SetPreviewSelected(bool);
float DisplayScale();
void SetDisplayScale(float, bool persist = true);
PreviewStatus PreviewSnapshot();
// `described` carries the caller's already-read extents; null means query them on demand.
HRESULT Capture11(ID3D11DeviceContext*, const Input11&, const ResourceInfo* described);
void PreviewResult(HRESULT);
void RetireCompleted();
void ReleaseNativeCapture(ID3D11Device*);
void ReleaseGameStream11(UINT64);
void NativeFrame11(IDXGISwapChain*, ID3D11DeviceContext*);
bool NativeCaptureEnabled();
void SetNativeCaptureEnabled(bool);
bool DepthContrast();
void SetDepthContrast(bool, bool persist = true);
bool DepthFlip();
void SetDepthFlip(bool, bool persist = true);
DenseFlowSettings FlowOptions();
void SetFlowOptions(DenseFlowSettings, bool persist = true);
ComPtr<ID3D11ShaderResourceView> Image11(ID3D11Device*);
FARPROC ResolveNgx(HMODULE, LPCSTR, FARPROC);
}

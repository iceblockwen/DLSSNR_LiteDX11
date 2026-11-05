// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <d3d11_4.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <memory>
#include <functional>
#include "gpu_ticket.hpp"
namespace lite::nr
{
using Microsoft::WRL::ComPtr;
inline void ThrowIfFailed(HRESULT hr)
{
    if (FAILED(hr))
    {
        throw hr;
    }
}
inline float Clamped(float v, float lo, float hi, float fallback)
{
    return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
}
enum class InputOrigin : unsigned
{
    Unknown,
    GameDlss,
    NativeCapture
};
struct Exposure
{
    bool sceneLinear{}, prePresent{}, exposurePresent{};
    float pre = 1, scale = 1;
};
struct Guides
{
    float motionScaleX = 1, motionScaleY = 1;
    float displayScale = 1;
    bool depthInverted{}, flipDepth{}, depthContrast{}; // depthContrast affects preview only
    bool motionResolutionKnown = true;                  // explicit/native inputs know their encoding; NGX requires create flags
    unsigned originX[3]{}, originY[3]{}, width[3]{}, height[3]{};
};
struct Input11
{
    ID3D11Resource* resources[3]{}; // post-upscale color, depth, motion; borrowed for this call
    Guides guides;
    Exposure exposure;
    ID3D11Resource* exposureResource{};
    bool reset{};
    InputOrigin origin = InputOrigin::Unknown;
    float frameTimeMs = 16.6667f; // Presentation interval, used only by generated-MV visualization.
    UINT64 stream{};              // Unique NGX creation generation; zero is native fallback.
};
struct Frame12
{
    std::array<ComPtr<ID3D12Resource>, 3> resources;
    Guides guides;
    std::shared_ptr<GpuTicket> ticket;
    ComPtr<ID3D12Resource> exposureTexture;
    // Shared output twin opened for DX12. When set, the resolve writes it in place and the bridge
    // skips its own full-frame result copy. Null when the format or device cannot support it.
    ComPtr<ID3D12Resource> colorOutput;
    // Bridge11 submits this list on its own queue, including result copies
    // before later NR writes. This is not a game-owned command queue.
    ComPtr<ID3D12CommandQueue> orderedQueue;
    InputOrigin origin = InputOrigin::Unknown;
    bool generatedMotion{};
    float frameTimeMs = 16.6667f;
    bool converted[3]{}; // actual bridge format conversion, not an unchanged copy
};
DXGI_FORMAT ViewFormat(DXGI_FORMAT format, bool depth = false);
// Copy/share-compatible storage format: depth and sRGB variants become typeless.
DXGI_FORMAT StorageFormat(DXGI_FORMAT format);
bool ShaderViewCompatible(DXGI_FORMAT format, bool depth = false);
void Transition(ID3D12GraphicsCommandList*, ID3D12Resource*, D3D12_RESOURCE_STATES, D3D12_RESOURCE_STATES);
// Independent of NGX and of the panel. Descriptors/output belong to one GPU recording.
class Preview12
{
  public:
    explicit Preview12(ID3D12Device*);
    // All three inputs side by side: three tiles copied into one image.
    HRESULT Record(ID3D12GraphicsCommandList*, const Frame12&);
    ComPtr<ID3D12Resource> image;

  private:
    HRESULT RecordTile(ID3D12GraphicsCommandList*, const Frame12&, unsigned input, unsigned outputWidth,
                       unsigned outputHeight);
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12DescriptorHeap> heap;
    std::array<std::unique_ptr<Preview12>, 3> tiles;
};
// Lite's same-adapter shared twins + two command allocator slots + GPU fence handoff.
// Caller serializes the immediate context; no CPU wait is used by Process.
// Bridges on the same context share a device, queue and ordered fence timeline;
// each bridge retains its own slots and recordings. Both DX11 Flush calls remain.
class Bridge11
{
  public:
    explicit Bridge11(ID3D11DeviceContext*);
    ~Bridge11();
    using Recorder = std::function<HRESULT(ID3D12GraphicsCommandList*, const Frame12&, std::shared_ptr<void>&,
                                           ID3D12Resource*&, D3D12_RESOURCE_STATES&)>;
    // Recording callback owns its descriptors/auxiliary GPU resources in keepAlive.
    HRESULT Process(const Input11&, unsigned width, unsigned height, DXGI_FORMAT outputFormat, const Recorder&);
    ComPtr<ID3D11ShaderResourceView> Image() const;
    // Optional Debug output, queued only inside Recorder and owned by keepAlive.
    void QueuePreview(ID3D12Resource*, D3D12_RESOURCE_STATES);
    ComPtr<ID3D11ShaderResourceView> PreviewImage() const;
    ID3D12Device* Device() const;
    const wchar_t* LastOperation() const;
    // Verbose only: GPU milliseconds of the last completed recording, 0 when not measured.
    float ChainMilliseconds() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
HRESULT RecordDebug(Bridge11&, const Input11&);
}

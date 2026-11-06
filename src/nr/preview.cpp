// SPDX-License-Identifier: GPL-3.0-or-later
// Lite shaders/dlssnr.hlsl guide debug modes, separated from the model pass.
#include "frame.hpp"
#include "nr_shaders.hpp"
#include <cmath>
#include <stdexcept>
namespace lite::nr
{
DXGI_FORMAT ViewFormat(DXGI_FORMAT f, bool depth)
{
    switch (f)
    {
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:
            return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
            return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case DXGI_FORMAT_R16_TYPELESS:
            return depth ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R16_FLOAT;
        case DXGI_FORMAT_D16_UNORM:
            return DXGI_FORMAT_R16_UNORM;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R16G16_TYPELESS:
            return DXGI_FORMAT_R16G16_FLOAT;
        case DXGI_FORMAT_R32G32_TYPELESS:
            return DXGI_FORMAT_R32G32_FLOAT;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
            return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case DXGI_FORMAT_R32G32B32_TYPELESS:
            return DXGI_FORMAT_R32G32B32_FLOAT;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        default:
            return f;
    }
}
bool ShaderViewCompatible(DXGI_FORMAT f, bool depth)
{
    if (ViewFormat(f, depth) == f)
    {
        return true;
    }
    switch (f)
    {
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_R16G16_TYPELESS:
        case DXGI_FORMAT_R32G32_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        case DXGI_FORMAT_R32G32B32_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
            return true;
        default:
            return false;
    }
}
DXGI_FORMAT StorageFormat(DXGI_FORMAT f)
{
    switch (f)
    {
        case DXGI_FORMAT_D32_FLOAT:
            return DXGI_FORMAT_R32_TYPELESS;
        case DXGI_FORMAT_D16_UNORM:
            return DXGI_FORMAT_R16_TYPELESS;
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
            return DXGI_FORMAT_R24G8_TYPELESS;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            return DXGI_FORMAT_R32G8X24_TYPELESS;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return DXGI_FORMAT_R8G8B8A8_TYPELESS;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return DXGI_FORMAT_B8G8R8A8_TYPELESS;
        default:
            return f;
    }
}
void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    if (!r || before == after)
    {
        return;
    }
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list->ResourceBarrier(1, &b);
}
namespace
{

}
Preview12::Preview12(ID3D12Device* d) : device(d)
{
    D3D12_DESCRIPTOR_RANGE ranges[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
                                     {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1}};
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = {2, ranges};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = {0, 0, 12};
    D3D12_ROOT_SIGNATURE_DESC desc{2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> code, errors;
    ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &code, &errors));
    ThrowIfFailed(device->CreateRootSignature(0, code->GetBufferPointer(), code->GetBufferSize(), IID_PPV_ARGS(&root)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = root.Get();
    pso.CS = {litePreviewShader, sizeof(litePreviewShader)};
    ThrowIfFailed(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipeline)));
    D3D12_DESCRIPTOR_HEAP_DESC h{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    ThrowIfFailed(device->CreateDescriptorHeap(&h, IID_PPV_ARGS(&heap)));
}
HRESULT Preview12::Record(ID3D12GraphicsCommandList* list, const Frame12& frame)
{
    if (!list)
    {
        return E_INVALIDARG;
    }
    if (!frame.resources[0])
    {
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }
    auto d = frame.resources[0]->GetDesc();
    const unsigned w = frame.guides.width[0] ? frame.guides.width[0] : unsigned(d.Width), h = frame.guides.height[0] ? frame.guides.height[0] : d.Height;
    if (!w || !h || w > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION / 3)
    {
        return E_INVALIDARG;
    }
    if (!image || image->GetDesc().Width != w * 3 || image->GetDesc().Height != h)
    {
        d.Alignment = 0;
        d.Width = w * 3;
        d.Height = h;
        d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc = {1, 0};
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        auto hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&image));
        if (FAILED(hr))
        {
            return hr;
        }
    }
    Transition(list, image.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    HRESULT error = S_OK;
    for (unsigned n = 0; n < 3; ++n)
    {
        // Missing tiles are never displayed; keep available previews usable.
        // This does not synthesize a depth/MV resource for model processing.
        if (!frame.resources[n])
        {
            continue;
        }
        if (!tiles[n])
        {
            tiles[n] = std::make_unique<Preview12>(device.Get());
        }
        // Tile 2 keeps the caller's display scale; the others are shown at 1:1.
        auto guides = frame.guides;
        if (n != 2)
        {
            guides.displayScale = 1;
        }
        const auto hr = tiles[n]->RecordTile(list, frame.resources[n].Get(), guides, frame.generatedMotion,
                                             frame.frameTimeMs, n, w, h);
        if (FAILED(hr))
        {
            error = hr;
            continue;
        }
        auto* tile = tiles[n]->image.Get();
        Transition(list, tile, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION from{}, to{};
        from.pResource = tile;
        to.pResource = image.Get();
        from.Type = to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&to, w * n, 0, 0, &from, nullptr);
        Transition(list, tile, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    Transition(list, image.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return error;
}
// One tile: colour (0), depth (1) or motion vectors (2), scaled to the caller's extents.
HRESULT Preview12::RecordTile(ID3D12GraphicsCommandList* list, ID3D12Resource* source, const Guides& g,
                              bool generatedMotion, float frameTimeMs, unsigned input, unsigned outputWidth,
                              unsigned outputHeight)
{
    if (!list || input > 2)
    {
        return E_INVALIDARG;
    }
    if (!source)
    {
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }
    const auto desc = source->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != 1 ||
        desc.MipLevels != 1 || desc.SampleDesc.Count != 1 || (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
    {
        return E_NOTIMPL;
    }
    if (!ShaderViewCompatible(desc.Format, input == 1))
    {
        return E_NOTIMPL;
    }
    const unsigned sw = g.width[input] ? g.width[input] : unsigned(desc.Width);
    const unsigned sh = g.height[input] ? g.height[input] : desc.Height;
    if (!sw || !sh || g.originX[input] > desc.Width || sw > desc.Width - g.originX[input] ||
        g.originY[input] > desc.Height || sh > desc.Height - g.originY[input] ||
        !std::isfinite(g.motionScaleX) || !std::isfinite(g.motionScaleY) ||
        !std::isfinite(g.displayScale) || g.displayScale <= 0)
    {
        return E_INVALIDARG;
    }
    const unsigned ow = outputWidth ? outputWidth : sw, oh = outputHeight ? outputHeight : sh;
    if (!image || image->GetDesc().Width != ow || image->GetDesc().Height != oh)
    {
        auto out = desc;
        out.Alignment = 0;
        out.Width = ow;
        out.Height = oh;
        out.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        out.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        out.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        D3D12_HEAP_PROPERTIES properties{};
        properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        auto hr = device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &out,
                                                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&image));
        if (FAILED(hr))
        {
            return hr;
        }
    }
    auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = ViewFormat(desc.Format, input == 1);
    if (input == 2 && srv.Format != DXGI_FORMAT_R16G16_FLOAT && srv.Format != DXGI_FORMAT_R32G32_FLOAT &&
        srv.Format != DXGI_FORMAT_R16G16_SNORM)
    {
        return E_NOTIMPL;
    }
    if (input == 1 && srv.Format != DXGI_FORMAT_R32_FLOAT && srv.Format != DXGI_FORMAT_R16_FLOAT && srv.Format != DXGI_FORMAT_R16_UNORM &&
        srv.Format != DXGI_FORMAT_R24_UNORM_X8_TYPELESS && srv.Format != DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS)
    {
        return E_NOTIMPL;
    }
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{srv.Format};
    auto hr = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
    if (FAILED(hr) || !(support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD))
    {
        return E_NOTIMPL;
    }
    device->CreateShaderResourceView(source, &srv, cpu);
    cpu.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(image.Get(), nullptr, &uav, cpu);
    struct Constants
    {
        unsigned mode, ox, oy, sw, sh, ow, oh, flags;
        float sx, sy, gain, pad;
    };
    const Constants c{input + 1u, g.originX[input], g.originY[input], sw, sh, ow, oh,
                      (g.depthInverted ? 1u : 0u) | (g.flipDepth ? 2u : 0u) | (g.depthContrast ? 4u : 0u) |
                          (generatedMotion ? 8u : 0u),
                      g.motionScaleX, g.motionScaleY, g.displayScale,
                      std::isfinite(frameTimeMs) && frameTimeMs > 0 ? frameTimeMs : 16.6667f};
    Transition(list, image.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto* h = heap.Get();
    list->SetDescriptorHeaps(1, &h);
    list->SetPipelineState(pipeline.Get());
    list->SetComputeRootSignature(root.Get());
    list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRoot32BitConstants(1, 12, &c, 0);
    list->Dispatch((ow + 7) / 8, (oh + 7) / 8, 1);
    Transition(list, image.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return S_OK;
}
HRESULT RecordDebug(Bridge11& bridge, const Input11& input)
{
    if (!input.resources[0])
    {
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }
    ComPtr<ID3D11Texture2D> texture;
    auto hr = input.resources[0]->QueryInterface(IID_PPV_ARGS(&texture));
    if (FAILED(hr))
    {
        return hr;
    }
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    const unsigned w = input.guides.width[0] ? input.guides.width[0] : desc.Width;
    const unsigned h = input.guides.height[0] ? input.guides.height[0] : desc.Height;
    return bridge.Process(input, w * 3, h, DXGI_FORMAT_R8G8B8A8_UNORM,
                          [&](ID3D12GraphicsCommandList* list, const Frame12& frame, std::shared_ptr<void>& keep,
                              ID3D12Resource*& result, D3D12_RESOURCE_STATES& state)
                          {
                              auto renderer = std::static_pointer_cast<Preview12>(keep);
                              if (!renderer)
                              {
                                  renderer = std::make_shared<Preview12>(bridge.Device());
                                  keep = renderer;
                              }
                              auto code = renderer->Record(list, frame);
                              result = renderer->image.Get();
                              state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                              return code;
                          });
}
}

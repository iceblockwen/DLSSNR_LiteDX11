// DX12 resource/recording adapter for the authorized optical-flow source port.
// Matching/filtering attribution is retained in shaders/dense_flow.hlsl.
#include "dense_flow.hpp"
#include "../graphics.hpp"
#include "dense_shader.hpp"
#include "nr_shaders.hpp"
#include <algorithm>
#include <array>
#include <map>
#include <cmath>
namespace lite::nr
{
namespace
{
enum Stage
{
    Prepare,
    DepthMip,
    Down,
    Match,
    Pool,
    Upscale,
    Finish,
    StageCount
};
struct Pipelines
{
    ComPtr<ID3D12RootSignature> root;
    std::array<ComPtr<ID3D12PipelineState>, StageCount> pso;
    explicit Pipelines(ID3D12Device* device)
    {
        D3D12_DESCRIPTOR_RANGE ranges[]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 7, 0, 0, 0}};
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants = {0, 0, 32};
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = {1, ranges};
        D3D12_STATIC_SAMPLER_DESC samplers[3]{};
        for (unsigned n = 0; n < 3; ++n)
        {
            auto& s = samplers[n];
            s.ShaderRegister = n;
            s.Filter = n == 2 ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            s.AddressU = s.AddressV = s.AddressW = n == 0 ? D3D12_TEXTURE_ADDRESS_MODE_MIRROR : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            s.MaxLOD = D3D12_FLOAT32_MAX;
            s.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        }
        D3D12_ROOT_SIGNATURE_DESC desc{2, params, 3, samplers, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> code, errors;
        ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &code, &errors));
        ThrowIfFailed(device->CreateRootSignature(0, code->GetBufferPointer(), code->GetBufferSize(), IID_PPV_ARGS(&root)));
        const D3D12_SHADER_BYTECODE pixels[]{
            {flowPrepareShader, sizeof(flowPrepareShader)}, {flowDepthMipShader, sizeof(flowDepthMipShader)}, {flowDownShader, sizeof(flowDownShader)}, {flowMatchShader, sizeof(flowMatchShader)}, {flowPoolShader, sizeof(flowPoolShader)}, {flowUpscaleShader, sizeof(flowUpscaleShader)}, {flowFinishShader, sizeof(flowFinishShader)}};
        static_assert(sizeof(pixels) / sizeof(pixels[0]) == StageCount);
        for (unsigned n = 0; n < StageCount; ++n)
        {
            {
                D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
                p.pRootSignature = root.Get();
                p.VS = {flowVertexShader, sizeof(flowVertexShader)};
                p.PS = pixels[n];
                p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
                p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                p.RasterizerState.DepthClipEnable = TRUE;
                p.SampleMask = ~0u;
                p.SampleDesc.Count = 1;
                p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                p.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
                p.DepthStencilState.FrontFace.StencilFunc = p.DepthStencilState.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
                p.DepthStencilState.FrontFace.StencilFailOp = p.DepthStencilState.FrontFace.StencilDepthFailOp = p.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
                p.DepthStencilState.BackFace = p.DepthStencilState.FrontFace;
                p.NumRenderTargets = n == Prepare || n == Down ? 2 : 1;
                p.RTVFormats[0] = n == Finish ? DXGI_FORMAT_R16G16_FLOAT : n < Match ? DXGI_FORMAT_R16_FLOAT
                                                                                     : DXGI_FORMAT_R16G16B16A16_FLOAT;
                p.RTVFormats[1] = n == Prepare || n == Down ? DXGI_FORMAT_R16_FLOAT : DXGI_FORMAT_UNKNOWN;
                for (unsigned rt = 0; rt < p.NumRenderTargets; ++rt)
                {
                    auto& blend = p.BlendState.RenderTarget[rt];
                    blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
                    blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
                    blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
                    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
                    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
                }
                ThrowIfFailed(device->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&pso[n])));
            }
        }
    }
};
ComPtr<ID3D12Resource> Make(ID3D12Device* device, unsigned w, unsigned h, DXGI_FORMAT format)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Format = format;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> texture;
    ThrowIfFailed(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&texture)));
    return texture;
}
struct Generation
{
    unsigned w{}, h{}, frames{};
    Guides guides;
    bool reversed{}, flip{}, noiseUploaded{};
    DenseFlowSettings settings;
    std::array<ComPtr<ID3D12Resource>, 8> current, previous, a, b;
    std::array<ComPtr<ID3D12Resource>, 4> depth;
    ComPtr<ID3D12Resource> prevDepth, quarter, half, uvMotion, noise, noiseUpload;
    std::map<ID3D12Resource*, D3D12_RESOURCE_STATES> states;
};
struct Recording
{
    std::shared_ptr<Generation> generation;
    std::shared_ptr<Pipelines> pipelines;
    ComPtr<ID3D12DescriptorHeap> heap, rtv;
    unsigned offset{};
};
void Move(ID3D12GraphicsCommandList* list, Generation& g, ID3D12Resource* resource, D3D12_RESOURCE_STATES to)
{
    auto& state = g.states[resource];
    Transition(list, resource, state, to);
    state = to;
}
struct Constants
{
    unsigned target[2], full[2], level;
    int radius, dilation;
    unsigned reversed, flip, first, logarithmic;
    float farPlane, fov, multiplier, reserved;
    unsigned colorOrigin[2], depthOrigin[2], depthSize[2], padding[2];
    float scale[2], offset[2], pixelOffset[2], mappingPad[2];
    unsigned reserved2;
};
static_assert(sizeof(Constants) == 128);
}
struct DenseFlow12::Impl
{
    ComPtr<ID3D12Device> device;
    std::shared_ptr<Pipelines> pipelines;
    std::shared_ptr<Generation> generation;
    explicit Impl(ID3D12Device* d) : device(d), pipelines(std::make_shared<Pipelines>(d))
    {
    }
    void Ensure(unsigned w, unsigned h, const Guides& guides, const DenseFlowSettings& settings)
    {
        if (generation && generation->w == w && generation->h == h)
        {
            if (generation->reversed != guides.depthInverted || generation->flip != guides.flipDepth)
            {
                // Convention changed: the recorded history used the other convention, but the
                // textures are still the right size. Restarting the history is enough; freeing
                // and reallocating every texture here stalled whenever the chosen depth changed.
                generation->reversed = guides.depthInverted;
                generation->flip = guides.flipDepth;
                generation->frames = 0;
            }
            generation->settings = settings;
            generation->guides = guides;
            return;
        }
        auto g = std::make_shared<Generation>();
        g->w = w;
        g->h = h;
        g->guides = guides;
        g->reversed = guides.depthInverted;
        g->flip = guides.flipDepth;
        g->settings = settings;
        for (unsigned n = 0; n < 8; ++n)
        {
            const auto pw = std::max(1u, w >> n), ph = std::max(1u, h >> n);
            g->current[n] = Make(device.Get(), pw, ph, DXGI_FORMAT_R16_FLOAT);
            g->previous[n] = Make(device.Get(), pw, ph, DXGI_FORMAT_R16_FLOAT);
            const auto shift = std::max(3u, n);
            g->a[n] = Make(device.Get(), std::max(1u, w >> shift), std::max(1u, h >> shift), DXGI_FORMAT_R16G16B16A16_FLOAT);
            if (n)
            {
                g->b[n] = Make(device.Get(), std::max(1u, w >> shift), std::max(1u, h >> shift), DXGI_FORMAT_R16G16B16A16_FLOAT);
            }
        }
        for (unsigned n = 0; n < 4; ++n)
        {
            g->depth[n] = Make(device.Get(), std::max(1u, w >> n), std::max(1u, h >> n), DXGI_FORMAT_R16_FLOAT);
        }
        g->prevDepth = Make(device.Get(), std::max(1u, w >> 3), std::max(1u, h >> 3), DXGI_FORMAT_R16_FLOAT);
        g->quarter = Make(device.Get(), std::max(1u, w >> 2), std::max(1u, h >> 2), DXGI_FORMAT_R16G16B16A16_FLOAT);
        g->half = Make(device.Get(), std::max(1u, w >> 1), std::max(1u, h >> 1), DXGI_FORMAT_R16G16B16A16_FLOAT);
        // uvMotion is the only motion output: the pixel-space copy and the R8 loss mask fed
        // nothing but the removed debug view, so neither texture is allocated any more.
        g->uvMotion = Make(device.Get(), w, h, DXGI_FORMAT_R16G16_FLOAT);
        g->noise = Make(device.Get(), 256, 256, DXGI_FORMAT_R8_UNORM);
        D3D12_RESOURCE_DESC upload{};
        upload.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        upload.Width = sizeof(FlowNoise);
        upload.Height = 1;
        upload.DepthOrArraySize = upload.MipLevels = 1;
        upload.SampleDesc.Count = 1;
        upload.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        ThrowIfFailed(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &upload, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g->noiseUpload)));
        void* data{};
        D3D12_RANGE none{};
        ThrowIfFailed(g->noiseUpload->Map(0, &none, &data));
        memcpy(data, FlowNoise, sizeof(FlowNoise));
        g->noiseUpload->Unmap(0, nullptr);
        generation = std::move(g);
    }
    void Copy(ID3D12GraphicsCommandList* list, ID3D12Resource* from, ID3D12Resource* to)
    {
        auto& g = *generation;
        Move(list, g, from, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Move(list, g, to, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(to, from);
        Move(list, g, from, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Move(list, g, to, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    void Dispatch(ID3D12GraphicsCommandList* list, Recording& record, Stage stage, unsigned w, unsigned h, unsigned level, int radius, int dilation,
                  std::array<ID3D12Resource*, 7> inputs, std::initializer_list<ID3D12Resource*> outputs)
    {
        auto& g = *generation;
        const auto stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto cpu = record.heap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += uint64_t(record.offset) * stride;
        auto gpu = record.heap->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += uint64_t(record.offset) * stride;
        if (record.offset + inputs.size() > 512)
        {
            throw E_UNEXPECTED;
        }
        for (unsigned n = 0; n < inputs.size(); ++n)
        {
            auto* r = inputs[n] ? inputs[n] : g.noise.Get();
            // External bridge inputs are already SRV-readable; internal maps track their own states.
            if (stage != Prepare || n >= 2)
            {
                Move(list, g, r, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = ViewFormat(r->GetDesc().Format, stage == Prepare && n == 1);
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(r, &view, cpu);
            cpu.ptr += stride;
        }
        unsigned count = 0;
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 2> targets{};
        auto rt = record.rtv->GetCPUDescriptorHandleForHeapStart();
        const auto rtStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        rt.ptr += uint64_t(record.offset) * rtStride;
        for (auto* r : outputs)
        {
            Move(list, g, r, D3D12_RESOURCE_STATE_RENDER_TARGET);
            device->CreateRenderTargetView(r, nullptr, rt);
            targets[count] = rt;
            rt.ptr += rtStride;
            ++count;
        }
        record.offset += unsigned(inputs.size());
        Constants constants{{w, h}, {g.w, g.h}, level, radius, dilation, unsigned(g.reversed), unsigned(g.flip), unsigned(!g.frames), unsigned(g.settings.logarithmic), g.settings.farPlane, g.settings.fov, g.settings.depthMultiplier, 0, {g.guides.originX[0], g.guides.originY[0]}, {g.guides.originX[1], g.guides.originY[1]}, {g.guides.width[1], g.guides.height[1]}, {0, 0}, {g.settings.depthScaleX, g.settings.depthScaleY}, {g.settings.depthOffsetX, g.settings.depthOffsetY}, {g.settings.depthPixelOffsetX, g.settings.depthPixelOffsetY}, {0, 0}, 0};
        auto* heap = record.heap.Get();
        list->SetDescriptorHeaps(1, &heap);
        list->SetPipelineState(pipelines->pso[stage].Get());
        list->SetGraphicsRootSignature(pipelines->root.Get());
        list->SetGraphicsRoot32BitConstants(0, 32, &constants, 0);
        list->SetGraphicsRootDescriptorTable(1, gpu);
        D3D12_VIEWPORT viewport{0, 0, float(w), float(h), 0, 1};
        D3D12_RECT scissor{0, 0, LONG(w), LONG(h)};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->OMSetRenderTargets(count, targets.data(), FALSE, nullptr);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
        list->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
        for (auto* r : outputs)
        {
            Move(list, g, r, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
    }
};
DenseFlow12::DenseFlow12(ID3D12Device* device) : impl(std::make_unique<Impl>(device))
{
}
DenseFlow12::~DenseFlow12() = default;
void DenseFlow12::Reset()
{
    impl->generation.reset();
}
void DenseFlow12::InvalidateHistory()
{
    if (impl->generation)
    {
        impl->generation->frames = 0;
    }
}
ComPtr<ID3D12Resource> DenseFlow12::MotionUV() const
{
    return impl->generation ? impl->generation->uvMotion : nullptr;
}
HRESULT DenseFlow12::Record(ID3D12GraphicsCommandList* list, const Frame12& frame, std::shared_ptr<void>& owned, const DenseFlowSettings& settings)
{
    try
    {
        if (!list || list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT || !frame.resources[0] || !frame.resources[1] || !std::isfinite(settings.fov) || settings.fov < 1 || settings.fov > 179 || !std::isfinite(settings.farPlane) || settings.farPlane < 2 || !std::isfinite(settings.depthMultiplier) || settings.depthMultiplier <= 0)
        {
            return E_INVALIDARG;
        }
        const auto desc = frame.resources[0]->GetDesc();
        const unsigned w = frame.guides.width[0] ? frame.guides.width[0] : unsigned(desc.Width), h = frame.guides.height[0] ? frame.guides.height[0] : desc.Height;
        if (!w || !h || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
        {
            return E_INVALIDARG;
        }
        for (unsigned n = 0; n < 2; ++n)
        {
            ComPtr<ID3D12Device> owner;
            ThrowIfFailed(frame.resources[n]->GetDevice(IID_PPV_ARGS(&owner)));
            if (owner.Get() != impl->device.Get())
            {
                return E_INVALIDARG;
            }
        }
        auto guides = frame.guides;
        for (unsigned n = 0; n < 2; ++n)
        {
            const auto d = frame.resources[n]->GetDesc();
            if (!guides.width[n])
            {
                guides.width[n] = unsigned(d.Width);
            }
            if (!guides.height[n])
            {
                guides.height[n] = d.Height;
            }
            if (guides.originX[n] > d.Width || guides.width[n] > d.Width - guides.originX[n] || guides.originY[n] > d.Height || guides.height[n] > d.Height - guides.originY[n])
            {
                return E_INVALIDARG;
            }
        }
        if (!std::isfinite(settings.depthScaleX) || settings.depthScaleX <= 0 || !std::isfinite(settings.depthScaleY) || settings.depthScaleY <= 0 || !std::isfinite(settings.depthOffsetX) || !std::isfinite(settings.depthOffsetY) || !std::isfinite(settings.depthPixelOffsetX) || !std::isfinite(settings.depthPixelOffsetY))
        {
            return E_INVALIDARG;
        }
        impl->Ensure(w, h, guides, settings);
        auto& g = *impl->generation;
        auto record = std::static_pointer_cast<Recording>(owned);
        if (!record)
        {
            record = std::make_shared<Recording>();
        }
        record->generation = impl->generation;
        record->pipelines = impl->pipelines;
        record->offset = 0;
        if (!record->heap)
        {
            D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 512, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
            ThrowIfFailed(impl->device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&record->heap)));
        }
        owned = record;
        if (!record->rtv)
        {
            D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 512, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
            ThrowIfFailed(impl->device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&record->rtv)));
        }
        if (!g.noiseUploaded)
        {
            Move(list, g, g.noise.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION to{};
            to.pResource = g.noise.Get();
            to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION from{};
            from.pResource = g.noiseUpload.Get();
            from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            from.PlacedFootprint.Footprint = {DXGI_FORMAT_R8_UNORM, 256, 256, 1, 256};
            list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            Move(list, g, g.noise.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            g.noiseUploaded = true;
        }
        auto dispatch = [&](Stage stage, unsigned shift, unsigned level, int radius, int dilation, std::array<ID3D12Resource*, 7> inputs, std::initializer_list<ID3D12Resource*> outputs)
        {
            impl->Dispatch(list, *record, stage, std::max(1u, w >> shift), std::max(1u, h >> shift), level, radius, dilation, inputs, outputs);
        };
        for (unsigned n = 0; n < 2; ++n)
        {
            Transition(list, frame.resources[n].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        dispatch(Prepare, 0, 0, 0, 0, {frame.resources[0].Get(), frame.resources[1].Get()}, {g.current[0].Get(), g.depth[0].Get()});
        for (unsigned n = 0; n < 2; ++n)
        {
            Transition(list, frame.resources[n].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        for (unsigned n = 1; n < 4; ++n)
        {
            dispatch(DepthMip, n, n, 0, 0, {g.depth[n - 1].Get()}, {g.depth[n].Get()});
        }
        if (!g.frames)
        {
            const float zero[4]{};
            auto clear = [&](ID3D12Resource* r)
            {
                Move(list, g, r, D3D12_RESOURCE_STATE_RENDER_TARGET);
                auto handle = record->rtv->GetCPUDescriptorHandleForHeapStart();
                impl->device->CreateRenderTargetView(r, nullptr, handle);
                list->ClearRenderTargetView(handle, zero, 0, nullptr);
                Move(list, g, r, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            };
            clear(g.previous[0].Get());
            clear(g.prevDepth.Get());
        }
        for (unsigned n = 1; n < 8; ++n)
        {
            dispatch(Down, n, n, 0, 0, {g.current[n - 1].Get(), g.previous[n - 1].Get()}, {g.current[n].Get(), g.previous[n].Get()});
        }
        for (int n = 7; n >= 0; --n)
        {
            auto* prior = n == 7 ? nullptr : g.b[n + 1].Get();
            const unsigned shift = std::max(3u, unsigned(n));
            dispatch(Match, shift, unsigned(n), 1, 1, {g.current[n].Get(), g.previous[n].Get(), prior, g.depth[3].Get(), g.prevDepth.Get(), g.noise.Get(), g.depth[2].Get()}, {g.a[n].Get()});
            if (n)
            {
                dispatch(Pool, shift, unsigned(n), 3, 3, {nullptr, nullptr, g.a[n].Get(), g.depth[3].Get(), g.prevDepth.Get()}, {g.b[n].Get()});
            }
        }
        dispatch(Upscale, 2, 2, 3, 2, {nullptr, nullptr, g.a[0].Get(), g.depth[2].Get()}, {g.quarter.Get()});
        dispatch(Upscale, 1, 1, 2, 2, {nullptr, nullptr, g.quarter.Get(), g.depth[1].Get()}, {g.half.Get()});
        dispatch(Finish, 0, 0, 1, 2, {nullptr, nullptr, g.half.Get(), g.depth[0].Get()}, {g.uvMotion.Get()});
        impl->Copy(list, g.current[0].Get(), g.previous[0].Get());
        impl->Copy(list, g.depth[3].Get(), g.prevDepth.Get());
        ++g.frames;
        return S_OK;
    }
    catch (HRESULT hr)
    {
        Reset();
        return hr;
    }
    catch (...)
    {
        Reset();
        return E_FAIL;
    }
}
HRESULT DenseFlow12::RecordForNR(ID3D12GraphicsCommandList* list, const Frame12& frame, std::shared_ptr<void>& owned, const DenseFlowSettings& settings)
{
    // The final upscale already writes uvMotion at frame resolution, which is the same
    // contract Launchpad hands to its consumer: UV-space motion plus a scale. Converting
    // it to pixels in a second full-screen pass cost one pass plus 41 MB of writes per
    // frame and lost precision in R16F for large displacements.
    return Record(list, frame, owned, settings);
}
}

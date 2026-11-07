// SPDX-License-Identifier: GPL-3.0-or-later
#include "pass.hpp"
#include "model_runtime.hpp"
#include "gpu_util.hpp"
#include "../graphics.hpp"
#include "nr_shaders.hpp"
#include <mutex>
#include <algorithm>
#include <cmath>
#include <tuple>
#include <vector>
namespace lite::nr
{
namespace
{
constexpr auto SRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
bool Plain(ID3D12Resource* r)
{
    if (!r)
    {
        return false;
    }
    auto d = r->GetDesc();
    return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.DepthOrArraySize == 1 &&
           d.MipLevels == 1 && d.SampleDesc.Count == 1 && d.Width && d.Height;
}
// Settings arrive validated by SetModelSettings; only game-provided values are bounded here.
void Tuning(ModelRuntime::Batch& backend, const PassSettings& s)
{
    backend.SetUnsigned("DLSSNR.Hint.Render.Preset", s.preset);
    backend.SetUnsigned("DLSSNR.Style", s.style);
    backend.SetFloat("DLSSNR.Intensity", s.intensity);
    backend.SetFloat("DLSSNR.LocalStructureStrength", s.structure);
    backend.SetFloat("DLSSNR.LocalToneStrength", s.tone);
    backend.SetFloat("DLSSNR.SkinStructureStrength", s.skin);
    backend.SetUnsigned("DLSSNR.UseAutoMask", s.autoMask ? 1u : 0u);
    backend.SetUnsigned("DLSSNR.UICorrection", s.uiCorrection ? 1u : 0u);
}
// One entry per NGX region name. The key strings are constants, so the per-frame
// parameter pass never builds a std::string.
struct RegionKeys
{
    const char* x;
    const char* y;
    const char* w;
    const char* h;
};
enum class RegionKind : unsigned
{
    Color,
    Output,
    Depth,
    MVec
};
constexpr RegionKeys kRegionKeys[]{
    {"DLSSNR.ColorSubrectBaseX", "DLSSNR.ColorSubrectBaseY", "DLSSNR.ColorSubrectWidth", "DLSSNR.ColorSubrectHeight"},
    {"DLSSNR.OutputSubrectBaseX", "DLSSNR.OutputSubrectBaseY", "DLSSNR.OutputSubrectWidth",
     "DLSSNR.OutputSubrectHeight"},
    {"DLSSNR.DepthSubrectBaseX", "DLSSNR.DepthSubrectBaseY", "DLSSNR.DepthSubrectWidth", "DLSSNR.DepthSubrectHeight"},
    {"DLSSNR.MVecSubrectBaseX", "DLSSNR.MVecSubrectBaseY", "DLSSNR.MVecSubrectWidth", "DLSSNR.MVecSubrectHeight"},
};
void Region(ModelRuntime::Batch& backend, RegionKind kind, unsigned w, unsigned h, unsigned x = 0, unsigned y = 0)
{
    const auto& keys = kRegionKeys[unsigned(kind)];
    backend.SetUnsigned(keys.x, x);
    backend.SetUnsigned(keys.y, y);
    backend.SetUnsigned(keys.w, w);
    backend.SetUnsigned(keys.h, h);
}
ComPtr<ID3D12Resource> Texture(ID3D12Device* d, unsigned w, unsigned h, DXGI_FORMAT f)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Format = f;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> r;
    ThrowIfFailed(d->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, UAV, nullptr,
                                             IID_PPV_ARGS(&r)));
    return r;
}
// The re-typed motion-vector twin keeps the game's own precision: turning a 32-bit vector pair
// into half would quantise it, and Opt re-types within the same family for the same reason.
DXGI_FORMAT MotionFormat(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_R32G32_TYPELESS || f == DXGI_FORMAT_R32G32_FLOAT
               ? DXGI_FORMAT_R32G32_FLOAT
               : DXGI_FORMAT_R16G16_FLOAT;
}
struct ParkedFeature
{
    void* feature{};
    ModelRuntime* backend{};
    std::shared_ptr<GpuTicket> ticket;
};
std::mutex parkedMutex;
std::vector<ParkedFeature> parkedFeatures;
void ParkFeature(void* feature, ModelRuntime* backend, const std::shared_ptr<GpuTicket>& ticket)
{
    if (!feature || !backend)
    {
        return;
    }
    std::lock_guard lock(parkedMutex);
    parkedFeatures.push_back({feature, backend, ticket});
}
void DrainParkedFeatures()
{
    std::vector<ParkedFeature> retired;
    {
        std::lock_guard lock(parkedMutex);
        for (auto it = parkedFeatures.begin(); it != parkedFeatures.end();)
        {
            // Bridge11 alone submits the list once. Discarded means never
            // executed; failed GPU signals remain Submitted and incomplete.
            const bool safe = it->ticket &&
                              (it->ticket->Status() == GpuTicket::State::Discarded || it->ticket->Complete());
            if (!safe)
            {
                ++it;
                continue;
            }
            retired.push_back(*it);
            it = parkedFeatures.erase(it);
        }
    }
    for (auto& entry : retired)
    {
        entry.backend->Release(entry.feature);
    }
}
struct Layer
{
    ModelRuntime* backend{};
    void* feature{};
    std::shared_ptr<GpuTicket> last;
    bool first = true, failed{};
    ~Layer()
    {
        ParkFeature(feature, backend, last);
    }
};
struct Context
{
    unsigned w{}, h{};
    PassSettings tuning;
    DXGI_FORMAT format{};
    ComPtr<ID3D12Resource> proxy, answer, output;
    ComPtr<ID3D12Resource> guides[2];
    std::shared_ptr<Layer> layer;
    bool failed{}, outputReadable{};
    bool encoded = true; // colour goes through the HDR proxy instead of the game texture
};
struct TimingResources
{
    ComPtr<ID3D12QueryHeap> timestamps;
    ComPtr<ID3D12Resource> times;
    explicit TimingResources(ID3D12Device* device)
    {
        D3D12_QUERY_HEAP_DESC q{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 4, 0};
        ThrowIfFailed(device->CreateQueryHeap(&q, IID_PPV_ARGS(&timestamps)));
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = 4 * sizeof(UINT64);
        d.Height = d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ThrowIfFailed(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                      D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                      IID_PPV_ARGS(&times)));
    }
};
struct Recording
{
    std::shared_ptr<Context> context;
    std::shared_ptr<Layer> layer; // retain this recording's feature across tuning changes
    Frame12 input;
    ComPtr<ID3D12Resource> exposure;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12DescriptorHeap> heap;
    std::shared_ptr<WhiteReadback> meter;
    std::shared_ptr<TimingResources> timing;
    unsigned timedLayers{};
    bool evaluated{}, createdFeature{};
    UINT64 whiteEpoch{};
    bool sceneLinear{}, hasExposure{}, modelMeasurement{};
    unsigned liveExposure{}; // 0 off, 1 pre/exposure, 2 exposure/pre
    float preMultiplier = 1, fallbackWhite = 1;
    // Game-exposure ratio mapping used by the live GPU white path.
    float whiteExponent = 1, whiteTrim = 1;
    void RetireInputs()
    {
        if (input.ticket->Complete())
        {
            input.resources = {};
            input.exposureTexture.Reset();
            exposure.Reset();
            if (meter)
            {
                meter->RetireInputs();
            }
        }
    }
    void Timing(std::shared_ptr<TimingResources> resources)
    {
        if (!resources)
        {
            return;
        }
        timing = std::move(resources);
        input.ticket->RequestTiming();
    }
    void ReadTiming(PassStatus& status)
    {
        const auto frequency = input.ticket->TimestampFrequency();
        if (!timing || !frequency || !input.ticket->Complete())
        {
            return;
        }
        auto* times = timing->times.Get();
        void* data{};
        D3D12_RANGE range{0, 4 * sizeof(UINT64)};
        if (FAILED(times->Map(0, &range, &data)))
        {
            return;
        }
        auto* ticks = static_cast<UINT64*>(data);
        bool valid = ticks[1] >= ticks[0];
        for (unsigned n = 0; n < timedLayers; ++n)
        {
            valid = valid && ticks[3 + 2 * n] >= ticks[2 + 2 * n];
        }
        if (valid)
        {
            status.gpuMilliseconds = float(double(ticks[1] - ticks[0]) * 1000 / double(frequency));
            status.modelMilliseconds =
                timedLayers ? float(double(ticks[3] - ticks[2]) * 1000 / double(frequency)) : 0;
            status.timingValid = true;
        }
        D3D12_RANGE noWrite{};
        times->Unmap(0, &noWrite);
    }
};
bool SameTuning(const PassSettings& a, const PassSettings& b)
{
    return a.preset == b.preset && a.style == b.style && a.autoMask == b.autoMask &&
           a.uiCorrection == b.uiCorrection && a.intensity == b.intensity &&
           a.structure == b.structure && a.tone == b.tone && a.skin == b.skin;
}
}
bool LinearColorFormat(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R16G16B16A16_TYPELESS ||
           f == DXGI_FORMAT_R32G32B32A32_FLOAT || f == DXGI_FORMAT_R32G32B32A32_TYPELESS ||
           f == DXGI_FORMAT_R11G11B10_FLOAT || f == DXGI_FORMAT_R32G32B32_FLOAT;
}
void RetireModelFeatures()
{
    DrainParkedFeatures();
}
struct PassExecutor12::Impl
{
    ComPtr<ID3D12Device> device;
    ModelRuntime& backend;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    std::unique_ptr<WhiteMeter12> meter;
    WhitePoint white;
    std::shared_ptr<Context> context;
    std::shared_ptr<Recording> previous;
    std::vector<std::shared_ptr<Recording>> inFlight;
    std::vector<std::shared_ptr<TimingResources>> timingPool;
    // Shader-visible heaps retired by a completed fence. Keeping a few means a
    // steady-state frame never creates one, and never waits for the GPU either.
    static constexpr size_t kHeapPoolMax = 4;
    std::vector<ComPtr<ID3D12DescriptorHeap>> heapPool;
    mutable std::mutex mutex;
    PassStatus status;
    UINT64 tick{}, frameIndex{};
    UINT64 whiteEpoch{};
    bool initialized{}, attempted{};
    HRESULT initializationError = S_OK;
    // Per-input verdict of the identity, device and format checks. A bridge keeps
    // its textures until the slot is rebuilt, so this only invalidates there.
    struct Checked
    {
        ID3D12Resource* resource{};
        DXGI_FORMAT format{};
        UINT64 width{};
        UINT height{};
        HRESULT verdict = S_OK;
        const char* reason{};
    };
    std::array<Checked, 3> checked{};
    // Constant per device; queried once instead of on every descriptor write.
    UINT viewStride{};
    explicit Impl(ID3D12Device* d, ModelRuntime& b) : device(d), backend(b)
    {
        viewStride = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    // Restart temporal state without freeing the feature or its working textures.
    void RestartHistory()
    {
        if (context && context->layer)
        {
            context->layer->first = true;
        }
    }
    std::shared_ptr<TimingResources> AcquireTiming()
    {
        // Recordings retain these through submission and fence retirement. Sole
        // pool ownership also excludes any caller still retaining a completed frame.
        for (auto& resources : timingPool)
        {
            if (resources.use_count() == 1)
            {
                return resources;
            }
        }
        if (timingPool.size() >= 8)
        {
            return {}; // Optional diagnostics never wait for GPU.
        }
        auto resources = std::make_shared<TimingResources>(device.Get());
        timingPool.push_back(resources);
        return resources;
    }
    void Shaders()
    {
        if (root && pipeline)
        {
            return;
        }
        D3D12_DESCRIPTOR_RANGE ranges[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0, 0},
                                         {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 4}};
        // The shader only uses Load(), so the root signature needs no sampler.
        ComputePipeline(device.Get(), ranges, 2, 12, liteColorShader, sizeof(liteColorShader),
                        root.ReleaseAndGetAddressOf(), pipeline.ReleaseAndGetAddressOf());
    }
    HRESULT Record(ID3D12GraphicsCommandList* list, const Frame12& frame, const Exposure& exposure,
                   const PassSettings& settings, bool reset,
                   std::shared_ptr<void>& keep, ID3D12Resource*& result)
    {
        result = frame.resources[0].Get();
        status.edited = false;
        status.actualPasses = 0;
        status.frameSkipped = false;
        if (!settings.gpuTiming)
        {
            status.timingValid = false;
            // In-flight recordings retain their own queries until fence retirement.
            timingPool.clear();
        }
        ThrowIfFailed(device->GetDeviceRemovedReason());
        auto refuse = [&](HRESULT hr, const char* why)
        {
            status.error = hr;
            status.reason = why;
            status.ready = false;
            status.frameSkipped = true;
            status.failedPass = 0;
            status.ngxResult = 0;
            return hr;
        };
        if (!list || !frame.ticket)
        {
            return refuse(E_INVALIDARG, "Missing NR recording/submission contract");
        }
        if (!frame.guides.motionResolutionKnown)
        {
            return refuse(E_NOTIMPL,
                          "Missing NGX create flags; motion-vector resolution is unknown");
        }
        if (!initialized)
        {
            if (!attempted)
            {
                attempted = true;
                initializationError = backend.Initialize(device.Get());
            }
            if (FAILED(initializationError))
            {
                status.error = initializationError;
                status.frameSkipped = initializationError == E_NOTIMPL;
                status.reason = "NR runtime unavailable for this device; original frame preserved";
                return initializationError;
            }
            initialized = true;
        }
        status.error = S_OK;
        if (!backend.Capability())
        {
            status.reason = "NGX capability block unavailable";
            return E_POINTER;
        }
        // A bridge slot keeps its twins for as long as the pointer and description
        // stay the same, so the COM and format checks run once per distinct input.
        auto inspect = [&](unsigned n, ID3D12Resource* r, const D3D12_RESOURCE_DESC& desc)
        {
            if (!Plain(r))
            {
                return std::pair{HRESULT(E_NOTIMPL), "NR needs color, depth and motion textures"};
            }
            ComPtr<ID3D12Device> owner;
            ThrowIfFailed(r->GetDevice(IID_PPV_ARGS(&owner)));
            if (owner.Get() != device.Get())
            {
                return std::pair{HRESULT(E_INVALIDARG), "NR resource belongs to another device"};
            }
            if ((desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) ||
                !ShaderViewCompatible(desc.Format, n == 1))
            {
                return std::pair{HRESULT(E_NOTIMPL), "NR input has no compatible shader view"};
            }
            const auto view = ViewFormat(desc.Format, n == 1);
            if (n == 2 && view != DXGI_FORMAT_R16G16_FLOAT && view != DXGI_FORMAT_R32G32_FLOAT &&
                view != DXGI_FORMAT_R16G16_SNORM)
            {
                return std::pair{HRESULT(E_NOTIMPL), "Unsupported motion-vector encoding"};
            }
            if (n == 1 && view != DXGI_FORMAT_R32_FLOAT && view != DXGI_FORMAT_R16_FLOAT &&
                view != DXGI_FORMAT_R16_UNORM && view != DXGI_FORMAT_R24_UNORM_X8_TYPELESS &&
                view != DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS)
            {
                return std::pair{HRESULT(E_NOTIMPL), "Unsupported depth encoding"};
            }
            return std::pair{HRESULT(S_OK), static_cast<const char*>(nullptr)};
        };
        for (unsigned n = 0; n < 3; ++n)
        {
            auto* r = frame.resources[n].Get();
            auto desc = r->GetDesc();
            auto& known = checked[n];
            if (known.resource != r || known.format != desc.Format || known.width != desc.Width ||
                known.height != desc.Height)
            {
                known = {r, desc.Format, desc.Width, desc.Height};
                std::tie(known.verdict, known.reason) = inspect(n, r, desc);
            }
            if (FAILED(known.verdict))
            {
                return refuse(known.verdict, known.reason);
            }
            // The game declares these every frame, so the bounds stay per frame.
            auto& g = frame.guides;
            const auto w = g.width[n] ? g.width[n] : unsigned(desc.Width),
                       h = g.height[n] ? g.height[n] : desc.Height;
            if (g.originX[n] > desc.Width || w > desc.Width - g.originX[n] ||
                g.originY[n] > desc.Height || h > desc.Height - g.originY[n])
            {
                return refuse(E_INVALIDARG, "NR guide subrect exceeds its texture");
            }
        }
        ComPtr<ID3D12Device> owner;
        ThrowIfFailed(list->GetDevice(IID_PPV_ARGS(&owner)));
        if (owner.Get() != device.Get() || list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
        {
            return refuse(E_INVALIDARG, "NR needs a direct command list from its device");
        }
        if (!std::isfinite(frame.guides.motionScaleX) || !std::isfinite(frame.guides.motionScaleY))
        {
            return refuse(E_INVALIDARG, "Invalid NR motion-vector scale");
        }
        auto color = frame.resources[0]->GetDesc();
        const auto format = ViewFormat(color.Format);
        status.hasInput = true;
        status.inputOrigin = frame.origin;
        status.inputTime = GetTickCount64();
        status.generatedMotion = frame.generatedMotion;
        status.depthFlipped = frame.guides.flipDepth;
        status.motionScaleX = frame.guides.motionScaleX;
        status.motionScaleY = frame.guides.motionScaleY;
        for (unsigned n = 0; n < 3; ++n)
        {
            status.inputConverted[n] = frame.converted[n];
        }
        // These are the adapters required by this input. A skipped frame's
        // state/reason remains visible separately; it is not a successful run.
        status.inputConverted[1] |= frame.resources[1]->GetDesc().Format != DXGI_FORMAT_R32_FLOAT;
        status.inputConverted[2] |=
            frame.resources[2]->GetDesc().Format != DXGI_FORMAT_R16G16_FLOAT;
        status.sceneLinear = exposure.sceneLinear;
        status.preExposure = exposure.pre;
        status.exposureScale = exposure.scale;
        status.preExposurePresent = exposure.prePresent;
        status.exposureScalePresent = exposure.exposurePresent;
        auto canFollow = [&]
        {
            if (!(previous->evaluated || previous->createdFeature) ||
                !previous->input.ticket->SubmittedOn(frame.orderedQueue.Get()) ||
                !context || context->failed)
            {
                return false;
            }
            if (context->w != color.Width || context->h != color.Height ||
                context->format != format || !SameTuning(context->tuning, settings))
            {
                return false;
            }
            // Creation and guide reallocation still require retirement.
            if (!context->layer || !context->layer->feature || context->layer->failed)
            {
                return false;
            }
            for (unsigned n = 1; n < 3; ++n)
            {
                auto a = previous->input.resources[n]->GetDesc(), b = frame.resources[n]->GetDesc();
                if (a.Width != b.Width || a.Height != b.Height || a.Format != b.Format)
                {
                    return false;
                }
            }
            return true;
        };
        if (previous)
        {
            if (previous->input.ticket->Status() == GpuTicket::State::Discarded)
            {
                // Drop the invalid history, not submitted recordings still owned by inFlight
                // or the caller. The unsubmitted feature is retired separately.
                previous.reset();
                context.reset();
                white.Reset();
                ++whiteEpoch;
                status.gpuExposureValid = false;
                status.readyPasses = status.failedPass = 0;
                status.timingValid = false;
                return refuse(E_ABORT, "NR recording discarded; history rebuild on next frame");
            }
            const bool tuningRebuild = context && !SameTuning(context->tuning, settings) &&
                                       previous->input.ticket->SubmittedOn(frame.orderedQueue.Get());
            if (!previous->input.ticket->Complete() && !canFollow() && !tuningRebuild)
            {
                // Only work that cannot be ordered invalidates history: a recording submitted on
                // the queue this frame follows can be waited on, so a reallocated guide no longer
                // costs the accumulation.
                if (!previous->input.ticket->SubmittedOn(frame.orderedQueue.Get()))
                {
                    RestartHistory();
                }
                status.ready = false;
                status.reason = "Waiting for NR creation, rebuild or unknown-queue recording";
                return S_FALSE;
            }
        }
        const UINT64 now = GetTickCount64();
        float dt = tick ? float(now - tick) * .001f : 0;
        tick = now;
        if (reset)
        {
            white.Reset();
            ++whiteEpoch;
            status.gpuExposureValid = false;
        }
        auto retire = [&](const std::shared_ptr<Recording>& completed)
        {
            if (settings.gpuTiming && completed->context == context)
            {
                completed->ReadTiming(status);
            }
            if (completed->meter && (completed->modelMeasurement || settings.panelStatistics) &&
                completed->whiteEpoch == whiteEpoch && !reset)
            {
                status.whiteMeasurementError =
                    completed->meter->Consume(white, &status.gpuExposure);
                if (SUCCEEDED(status.whiteMeasurementError))
                {
                    status.gpuExposureValid =
                        completed->hasExposure && std::isfinite(status.gpuExposure) &&
                        status.gpuExposure > 1e-6f && status.gpuExposure < 1e6f;
                    status.gpuDivisorValid = true;
                    float divisor = completed->fallbackWhite;
                    if (!completed->sceneLinear)
                    {
                        divisor = 1;
                    }
                    else if (completed->liveExposure && status.gpuExposureValid)
                    {
                        // The reverse ratio flips the fraction; the texture value itself is never inverted.
                        const float ratio =
                            completed->liveExposure == 2
                                ? status.gpuExposure / completed->preMultiplier
                                : completed->preMultiplier / status.gpuExposure;
                        divisor = Clamped(std::pow((std::max)(ratio, 1e-8f), completed->whiteExponent) *
                                              completed->whiteTrim,
                                          1e-8f, 1024, 1);
                    }
                    status.gpuDivisor = divisor;
                }
            }
            if (completed->heap && heapPool.size() < kHeapPoolMax)
            {
                heapPool.push_back(std::move(completed->heap));
            }
            completed->RetireInputs();
        };
        // Completion protects descriptor writes/readback/release, not same-queue execution.
        for (auto it = inFlight.begin(); it != inFlight.end();)
        {
            if ((*it)->input.ticket->Complete())
            {
                retire(*it);
                it = inFlight.erase(it);
            }
            else
            {
                ++it;
            }
        }
        if (previous)
        {
            if (previous->input.ticket->Complete())
            {
                retire(previous);
            }
            else
            {
                inFlight.push_back(previous);
            }
        }
        previous.reset();
        status.white = white.Resolve(settings.white, exposure, dt);
        if (!exposure.sceneLinear)
        {
            status.white.value = status.white.target = 1; // SDR bypass actually normalizes by one
        }
        // An SDR 8-bit input is copied into the HDR proxy unchanged, so the model can
        // read the game texture directly. The raw format is checked, not the shader
        // view: a TYPELESS or sRGB twin must not be handed to NGX as its colour input.
        const bool encoded = exposure.sceneLinear ||
                             (color.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
                              color.Format != DXGI_FORMAT_B8G8R8A8_UNORM);
        if (!context || context->w != color.Width || context->h != color.Height ||
            context->format != format || context->encoded != encoded)
        {
            D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
            ThrowIfFailed(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support,
                                                      sizeof(support)));
            if (!(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
            {
                return refuse(E_NOTIMPL, "Color output format cannot be written by NR resolve");
            }
            context = std::make_shared<Context>();
            context->w = unsigned(color.Width);
            context->h = color.Height;
            context->format = format;
            context->encoded = encoded;
            context->tuning = settings;
            white.Reset();
            ++whiteEpoch;
            frameIndex = 0;
            status.timingValid = false;
        }
        if (!SameTuning(context->tuning, settings))
        {
            context->layer.reset();
            context->tuning = settings;
            context->failed = false;
            frameIndex = 0;
            status.timingValid = false;
        }
        auto ctx = context;
        if (ctx->failed)
        {
            return status.error == S_OK ? E_FAIL : status.error;
        }
        status.actualPasses = 0;
        status.readyPasses = status.failedPass = 0;
        if (ctx->layer && ctx->layer->feature && !ctx->layer->failed)
        {
            status.readyPasses = 1;
        }
        if (reset && ctx->layer)
        {
            ctx->layer->first = true;
        }
        auto recording = std::make_shared<Recording>();
        recording->context = ctx;
        recording->input = frame;
        recording->exposure = frame.exposureTexture;
        recording->whiteEpoch = whiteEpoch;
        if (recording->exposure)
        {
            auto d = recording->exposure->GetDesc();
            ComPtr<ID3D12Device> exposureOwner;
            ThrowIfFailed(recording->exposure->GetDevice(IID_PPV_ARGS(&exposureOwner)));
            if (!Plain(recording->exposure.Get()) || d.Width != 1 || d.Height != 1 ||
                (d.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) ||
                !ShaderViewCompatible(d.Format) || exposureOwner.Get() != device.Get())
            {
                recording->exposure.Reset();
            }
        }
        status.gameExposureTexture = recording->exposure != nullptr;
        recording->hasExposure = status.gameExposureTexture;
        keep = recording; // registered before the first model/GPU operation
        if (!ctx->layer)
        {
            {
                // Creation-time configuration: the model reads these when the feature is
                // created, and every tuning change drops the layer before coming back here.
                // One lock for the whole group; Create below takes its own.
                ModelRuntime::Batch params(backend);
                params.SetUnsigned("DLSSNR.Enabled", 1u);
                params.SetUnsigned("DLSSNR.Width", ctx->w);
                params.SetUnsigned("DLSSNR.Height", ctx->h);
                params.SetUnsigned("CreationNodeMask", 1u);
                params.SetUnsigned("VisibilityNodeMask", 1u);
                Tuning(params, settings);
                params.SetResource("DLSSNR.ControlMask", nullptr);
            }
            // Validate resources before letting the model record initialization.
            Shaders();
            auto layer = std::make_shared<Layer>();
            layer->backend = &backend;
            layer->last = frame.ticket;
            ctx->layer = layer;
            recording->layer = layer;
            previous = recording;
            layer->first = true; // creation frame skips temporal evaluation
            const auto rc = backend.Create(list, &layer->feature);
            status.ngxResult = rc;
            status.ready = false;
            status.reason = "NR layer created; waiting for GPU submission and completion";
            if (rc != 1 || !layer->feature)
            {
                layer->failed = true;
                status.failedPass = 1;
                status.reason = "NR feature creation failed; original frame preserved";
                ctx->failed = true;
                status.error = E_FAIL;
                return E_FAIL;
            }
            recording->createdFeature = true;
            status.error = S_OK;
            return S_OK; // one creation per submitted frame; evaluate nothing on it
        }
        recording->layer = ctx->layer;
        recording->root = root;
        recording->pipeline = pipeline;
        if (!ctx->answer)
        {
            ctx->answer = Texture(device.Get(), ctx->w, ctx->h, DXGI_FORMAT_R16G16B16A16_FLOAT);
            if (ctx->encoded)
            {
                ctx->proxy = Texture(device.Get(), ctx->w, ctx->h, DXGI_FORMAT_R16G16B16A16_FLOAT);
            }
        }
        // A bridge that resolves in place into its shared output never needs this texture.
        if (!ctx->output && !frame.colorOutput)
        {
            // The output format was validated for UAV typed stores when this context was created.
            ctx->output = Texture(device.Get(), ctx->w, ctx->h, format);
        }
        if (settings.gpuTiming)
        {
            try
            {
                recording->Timing(AcquireTiming());
            }
            catch (HRESULT)
            {
                recording->timing.reset();
                status.timingValid = false;
            }
        } // optional diagnostics cannot disable NR
        if (!heapPool.empty())
        {
            recording->heap = std::move(heapPool.back());
            heapPool.pop_back();
        }
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 30,
                                      D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        if (!recording->heap)
        {
            ThrowIfFailed(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&recording->heap)));
        }
        auto cpu = [&](unsigned n)
        {
            auto h = recording->heap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += SIZE_T(n) * viewStride;
            return h;
        };
        auto srv = [&](unsigned n, ID3D12Resource* r, bool depth = false)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC v{};
            v.Format = r ? ViewFormat(r->GetDesc().Format, depth) : DXGI_FORMAT_R16G16B16A16_FLOAT;
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            v.Texture2D.MipLevels = 1;
            v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            device->CreateShaderResourceView(r, &v, cpu(n));
        };
        auto uav = [&](unsigned n, ID3D12Resource* r)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC v{};
            v.Format = r ? r->GetDesc().Format : DXGI_FORMAT_R16G16B16A16_FLOAT;
            v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            device->CreateUnorderedAccessView(r, nullptr, &v, cpu(n));
        };
        if (recording->timing)
        {
            list->EndQuery(recording->timing->timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        }
        srv(0, result);
        srv(1, nullptr);
        srv(2, nullptr);
        srv(3, recording->exposure.Get());
        uav(4, ctx->proxy.Get());
        uav(5, nullptr);
        // Lite's captured/shared input is already the owned, unedited image.
        // Reuse it for resolve instead of Lite's second half-float keep copy.
        // Without the proxy the game texture is both the model input and the resolve source.
        auto* const colour = ctx->encoded ? ctx->proxy.Get() : result;
        srv(6, colour);
        srv(8, result);
        srv(9, recording->exposure.Get());
        // A bridge that supplies its shared output lets the resolve write straight into it, so the
        // recording needs no result copy on its own queue.
        auto* const resolveTarget = frame.colorOutput ? frame.colorOutput.Get() : ctx->output.Get();
        uav(10, resolveTarget);
        uav(11, nullptr);
        struct Constants
        {
            unsigned mode, w, h, display;
            float white, preMul;
            unsigned live;
            float differenceGain;
            unsigned composed;
            float exponent, trim;
            unsigned reserved;
        } c{};
        c.w = ctx->w;
        c.h = ctx->h;
        c.display = exposure.sceneLinear ? 0u : 1u;
        c.white = status.white.value;
        c.preMul = 1;
        c.differenceGain = settings.differenceGain;
        c.composed = settings.composed ? 1u : 0u;
        c.exponent = settings.white.exponent;
        c.trim = settings.white.trim;
        const bool live =
            recording->exposure && exposure.sceneLinear && settings.white.useTexture &&
            settings.white.mode == WhiteMode::Game;
        if (live)
        {
            // 2 selects the swapped fraction; the shader keeps the texture value as is.
            c.live = settings.white.reverseRatio ? 2u : 1u;
            // The shader applies the exponent and trim, so only the raw pre-exposure is sent.
            // A missing or non-positive Pre.Exposure substitutes 1, matching the scalar path.
            const bool preUsable = exposure.prePresent && exposure.pre > 0;
            c.preMul = settings.white.usePreExposure && preUsable
                           ? Clamped(exposure.pre, 1e-8f, 1e6f, 1)
                           : 1;
            status.white.source = WhiteSource::Game;
        }
        recording->sceneLinear = exposure.sceneLinear;
        recording->liveExposure = c.live;
        recording->preMultiplier = c.preMul;
        recording->whiteExponent = c.exponent;
        recording->whiteTrim = c.trim;
        recording->fallbackWhite = c.white;
        auto run = [&](unsigned offset)
        {
            auto* heap = recording->heap.Get();
            list->SetDescriptorHeaps(1, &heap);
            list->SetComputeRootSignature(root.Get());
            list->SetPipelineState(pipeline.Get());
            auto gpu = heap->GetGPUDescriptorHandleForHeapStart();
            gpu.ptr += UINT64(offset) * viewStride;
            list->SetComputeRootDescriptorTable(0, gpu);
            list->SetComputeRoot32BitConstants(1, 12, &c, 0);
            list->Dispatch((ctx->w + 7) / 8, (ctx->h + 7) / 8, 1);
        };
        ID3D12Resource* guides[2]{frame.resources[1].Get(), frame.resources[2].Get()};
        for (unsigned n = 0; n < 2; ++n)
        {
            auto d = guides[n]->GetDesc();
            const auto expected = n ? MotionFormat(d.Format) : DXGI_FORMAT_R32_FLOAT;
            if (d.Format == expected)
            {
                continue;
            }
            auto& twin = ctx->guides[n];
            if (!twin || twin->GetDesc().Width != d.Width || twin->GetDesc().Height != d.Height ||
                twin->GetDesc().Format != expected)
            {
                twin = Texture(device.Get(), unsigned(d.Width), d.Height, expected);
            }
            else
            {
                Transition(list, twin.Get(), SRV, UAV);
            }
            const unsigned table = 12 + 6 * n;
            srv(table, guides[n], n == 0);
            for (unsigned j = 1; j < 4; ++j)
            {
                srv(table + j, nullptr);
            }
            uav(table + 4, twin.Get());
            uav(table + 5, nullptr);
            auto* heap = recording->heap.Get();
            list->SetDescriptorHeaps(1, &heap);
            list->SetComputeRootSignature(root.Get());
            list->SetPipelineState(pipeline.Get());
            auto gpu = heap->GetGPUDescriptorHandleForHeapStart();
            gpu.ptr += UINT64(table) * viewStride;
            list->SetComputeRootDescriptorTable(0, gpu);
            auto guideConstants = c;
            guideConstants.mode = 2;
            guideConstants.w = unsigned(d.Width);
            guideConstants.h = d.Height;
            list->SetComputeRoot32BitConstants(1, 12, &guideConstants, 0);
            list->Dispatch((guideConstants.w + 7) / 8, (guideConstants.h + 7) / 8, 1);
            Transition(list, twin.Get(), UAV, SRV);
            guides[n] = twin.Get();
        }
        // Measure unedited input even with a closed panel, only when NR needs measured white.
        // Game-mode white point runs entirely in the shader, so only Measured needs a readback.
        const bool modelMeasurement =
            exposure.sceneLinear && settings.white.mode == WhiteMode::Measured;
        recording->modelMeasurement = modelMeasurement;

        const bool measure = recording->modelMeasurement || settings.panelStatistics;
        if (!measure)
        {
            meter.reset(); // Recordings retain any submitted readback independently.
        }
        if (measure && (frameIndex++ % std::clamp(settings.white.every, 1u, 120u) == 0))
        {
            HRESULT hr{};
            try
            {
                if (!meter)
                {
                    meter = std::make_unique<WhiteMeter12>(device.Get());
                }
                hr = meter->Record(list, result, recording->meter, recording->exposure.Get(),
                                   frame.ticket);
            }
            catch (HRESULT error)
            {
                hr = error;
            }
            status.whiteMeasurementError = hr;
            if (FAILED(hr))
            {
                status.reason = "White meter unavailable; manual/history fallback";
            }
        }
        if (ctx->encoded)
        {
            run(0);
            Transition(list, ctx->proxy.Get(), UAV, SRV);
        }
        // One feature evaluates the immutable encoded input exactly once.
        bool readable{};
        ID3D12Resource* input = colour;
        ID3D12Resource* final{};
        BarrierBatch entry(list);
        if (status.readyPasses)
        {
            auto& layer = ctx->layer;
            auto* answer = ctx->answer.Get();
            {
                // One lock for the whole parameter group; Evaluate below takes its own.
                ModelRuntime::Batch params(backend);
                // Estimator distrust is a Debug guide, not the NR application mask.
                params.SetResource("DLSSNR.Color", input);
                params.SetResource("DLSSNR.Output", answer);
                params.SetUnsigned("DLSSNR.Reset", layer->first ? 1u : 0u);
                params.SetResource("DLSSNR.Depth", guides[0]);
                params.SetResource("DLSSNR.MVec", guides[1]);
                params.SetUnsigned("DLSSNR.DepthInverted", frame.guides.depthInverted ? 1u : 0u);
                params.SetFloat("DLSSNR.MVecScaleX", frame.guides.motionScaleX);
                params.SetFloat("DLSSNR.MVecScaleY", frame.guides.motionScaleY);
                Region(params, RegionKind::Color, ctx->w, ctx->h);
                Region(params, RegionKind::Output, ctx->w, ctx->h);
                for (unsigned g = 1; g < 3; ++g)
                {
                    auto d = frame.resources[g]->GetDesc();
                    Region(params, g == 1 ? RegionKind::Depth : RegionKind::MVec,
                           frame.guides.width[g] ? frame.guides.width[g] : unsigned(d.Width),
                           frame.guides.height[g] ? frame.guides.height[g] : d.Height,
                           frame.guides.originX[g], frame.guides.originY[g]);
                }
            }
            layer->last = frame.ticket;
            previous = recording; // retain even partially recorded model work
            if (recording->timing)
            {
                list->EndQuery(recording->timing->timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2);
            }
            const auto rc = backend.Evaluate(list, layer->feature);
            status.ngxResult = rc;
            if (recording->timing)
            {
                list->EndQuery(recording->timing->timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 3);
                recording->timedLayers = 1;
            }
            if (rc != 1)
            {
                layer->failed = true;
                status.failedPass = 1;
                status.readyPasses = 0;
            }
            else
            {
                entry.Add(answer, UAV, SRV);
                readable = true;
                final = answer;
                layer->first = false;
                status.actualPasses = 1;
            }
        }
        if (final)
        {
            srv(7, final);
            // The answer/SRV swap and the resolve input transition are adjacent, so
            // they go out as one barrier array before the resolve dispatch.
            if (frame.colorOutput)
            {
                entry.Add(resolveTarget, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            else if (ctx->outputReadable)
            {
                entry.Add(ctx->output.Get(), SRV, UAV);
            }
            entry.Flush(list);
            c.mode = 1;
            run(6);
            Transition(list, resolveTarget, UAV, SRV);
            if (!frame.colorOutput)
            {
                ctx->outputReadable = true;
            }
            result = resolveTarget;
            status.edited = status.ready = true;
            status.error = S_OK;
            recording->evaluated = true;
            status.reason = "NR evaluated successfully; resolve recorded";
        }
        else
        {
            ctx->failed = true;
            status.ready = false;
            status.error = E_FAIL;
            status.reason = "First NR layer evaluation failed; original frame preserved";
        }
        BarrierBatch tail(list);
        if (readable)
        {
            tail.Add(ctx->answer.Get(), SRV, UAV);
        }
        tail.Add(ctx->proxy.Get(), SRV, UAV);
        tail.Flush(list);
        if (recording->timing)
        {
            list->EndQuery(recording->timing->timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
            list->ResolveQueryData(recording->timing->timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                                   0, 2 + 2 * recording->timedLayers,
                                   recording->timing->times.Get(), 0);
        }
        previous = recording;
        return S_OK; // partial GPU work must still be submitted on failure
    }
};
PassExecutor12::PassExecutor12(ID3D12Device* d, ModelRuntime& b)
    : impl(std::make_unique<Impl>(d, b))
{
}
PassExecutor12::~PassExecutor12() = default;
HRESULT PassExecutor12::Record(ID3D12GraphicsCommandList* list, const Frame12& f, const Exposure& e,
                               const PassSettings& s, bool reset,
                               std::shared_ptr<void>& keep, ID3D12Resource*& result)
{
    std::lock_guard lock(impl->mutex);
    HRESULT hr = S_OK;
    DrainParkedFeatures();
    try
    {
        hr = impl->Record(list, f, e, s, reset, keep, result);
        if (FAILED(hr))
        {
            impl->status.error = hr;
            impl->status.ready = false;
        }
    }
    catch (HRESULT error)
    {
        hr = error;
        impl->status.error = hr;
        impl->status.ready = false;
        impl->status.reason = "NR resource operation failed; original frame preserved";
    }
    catch (...)
    {
        hr = E_FAIL;
        impl->status.error = E_FAIL;
        impl->status.ready = false;
    }
    return hr;
}
PassStatus PassExecutor12::Snapshot() const
{
    std::lock_guard lock(impl->mutex);
    return impl->status;
}
void PassExecutor12::InvalidateHistory()
{
    std::lock_guard lock(impl->mutex);
    impl->RestartHistory();
}
}

#include "capture11.hpp"
#include "../graphics.hpp"
#include "depth11.hpp"
#include "dense_flow.hpp"
#include "../config.hpp"
#include "model_settings.hpp"
#include "model_runtime.hpp"

#include <atomic>
#include <map>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <set>
namespace lite::nr
{
namespace
{
struct NativeCaptureState
{
    ComPtr<ID3D11Device> device;
    std::unique_ptr<Bridge11> bridge, generatedBridge;
    std::unique_ptr<DenseFlow12> flow;
    bool externallyFed{}, generatedLatest{};
    bool modelPreviewLatest{};
    std::unique_ptr<Bridge11> modelBridge;
    std::unique_ptr<PassExecutor12> model;
    std::unique_ptr<DenseFlow12> modelFlow;
    DenseFlowSettings modelFlowOptions;
    UINT64 revision{};
    UINT64 modelStream{};
    InputOrigin modelOrigin = InputOrigin::Unknown;
};
struct Registry
{
    std::recursive_mutex mutex;
    // Panel toggles arrive on the window thread while the frame lock is held for a whole
    // NR chain. A separate leaf lock keeps the publish path off that lock.
    std::mutex panelMutex;
    PreviewStatus status;
    DepthStatus depth;
    std::map<ID3D11DeviceContext*, NativeCaptureState> native;
    std::set<const void*> visiblePanels;
};
Registry& Global()
{
    static auto* r = new Registry;
    return *r;
} // pinned DLL; no GPU work under loader lock
std::atomic<bool> previewSelected{};
std::atomic<bool> panelVisible{};
std::atomic<bool> nativeCaptureEnabled{true};
std::atomic<bool> depthContrast{false};
std::atomic<bool> depthFlip{false};
DenseFlowSettings flowOptions;
std::atomic<float> gain{1};
std::atomic<bool> modelCached{};
void Report(HRESULT hr)
{
    auto& s = Global().status;
    s.error = hr;
    s.state = hr == S_FALSE ? State::Waiting : FAILED(hr) ? State::Failed
                                                          : State::Ready;
    const wchar_t* message = hr == S_FALSE ? L"GPU busy; original game frame kept"
                             : hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)
                                 ? L"Selected resource missing"
                             : hr == E_NOTIMPL    ? L"Unsupported texture or view format"
                             : hr == E_INVALIDARG ? L"Invalid resource, dimensions or subrect"
                             : FAILED(hr)         ? L"Resource capture/bridge failed"
                                                  : L"Frame resources captured";
    wcsncpy_s(s.reason, message, _TRUNCATE);
}
void Info(ID3D11Resource* const* resources, const Guides& guides,
          const ResourceInfo* described = nullptr)
{
    auto& s = Global().status;
    s.guides = guides;
    if (described)
    {
        for (unsigned n = 0; n < 3; ++n)
        {
            s.resources[n] = described[n];
        }
        return;
    }
    // Only the panel's Resources page reads these extents, so querying is panel demand.
    if (!PanelVisible())
    {
        return;
    }
    for (unsigned n = 0; n < 3; ++n)
    {
        s.resources[n] = {};
        if (!resources[n])
        {
            continue;
        }
        ComPtr<ID3D11Texture2D> t;
        if (SUCCEEDED(resources[n]->QueryInterface(IID_PPV_ARGS(&t))))
        {
            D3D11_TEXTURE2D_DESC d{};
            t->GetDesc(&d);
            s.resources[n] = {d.Width, d.Height, unsigned(d.Format), true};
        }
    }
}
void Save(const wchar_t* key, const wchar_t* value, const wchar_t* section = L"Diagnostics")
{
    QueueConfig(section, key, value);
}
void ResetPreviewHistory()
{
    auto& r = Global();
    for (auto& [context, native] : r.native)
    {
        (void)context;
        if (native.flow)
        {
            native.flow->Reset();
        }
        native.externallyFed = native.generatedLatest = false;
        if (!ModelActive())
        {
            depth::Release(native.device.Get());
        }
    }
}
// The native bridge hands the same DX12 executor its input. Its fences also order
// the copy back to the original DX11 texture, without waiting on the CPU.
HRESULT Model11(NativeCaptureState& native, ID3D11DeviceContext* context, const Input11& input, bool estimate,
                const ResourceInfo* described, ID3D11Resource* writeback = nullptr)
{
    if (!ModelActive())
    {
        return S_FALSE;
    }
    if (!input.resources[0] || !input.resources[1] || (!estimate && !input.resources[2]))
    {
        PassStatus s;
        s.reason = "Waiting for NR color, depth and motion";
        PublishPass(s);
        return S_FALSE;
    }
    if (!native.modelBridge)
    {
        context->GetDevice(&native.device);
        native.modelBridge = std::make_unique<Bridge11>(context);
    }
    auto* device = native.modelBridge->Device();
    auto hr = Runtime().Initialize(device);
    if (FAILED(hr))
    {
        return hr;
    }
    if (native.revision != ModelRevision())
    {
        native.model.reset();
        native.modelFlow.reset();
        native.revision = ModelRevision();
    }
    if (!native.model)
    {
        native.model = std::make_unique<PassExecutor12>(device, Runtime());
    }
    modelCached = true;
    // A caller that already supplied motion vectors never needs the estimator.
    const bool estimateMotion = estimate && !input.resources[2];
    if (estimateMotion && !native.modelFlow)
    {
        native.modelFlow = std::make_unique<DenseFlow12>(device);
    }
    const auto options = FlowOptions();
    if (estimateMotion && native.modelFlowOptions != options)
    {
        native.model->InvalidateHistory();
        native.modelFlowOptions = options;
    }
    D3D11_TEXTURE2D_DESC desc{};
    if (described && described[0].present)
    {
        desc.Width = described[0].width;
        desc.Height = described[0].height;
        desc.Format = DXGI_FORMAT(described[0].format);
    }
    else
    {
        ComPtr<ID3D11Texture2D> color;
        ThrowIfFailed(input.resources[0]->QueryInterface(IID_PPV_ARGS(&color)));
        color->GetDesc(&desc);
    }
    struct Payload
    {
        std::shared_ptr<void> flow, pass;
        std::shared_ptr<Preview12> preview;
    };
    bool previewRecorded = false;
    auto settings = GetModelSettings();
    // One bounded history per context. Never accumulate across feature generations
    // or between game NGX and native fallback, even at an identical resolution.
    const bool reset = input.reset || native.modelStream != input.stream || native.modelOrigin != input.origin;
    bool flowRecorded = false;
    auto adjusted = input;
    // The game's exposure texture only feeds Game white sampling and the panel readout.
    const auto& white = settings.pass.white;
    const bool exposureNeeded =
        settings.pass.panelStatistics ||
        (white.useTexture && adjusted.exposure.sceneLinear && white.mode == WhiteMode::Game);
    if (adjusted.exposureResource && !exposureNeeded)
    {
        adjusted.exposureResource = nullptr;
    }
    if (adjusted.exposureResource)
    {
        ComPtr<ID3D11Texture2D> texture;
        D3D11_TEXTURE2D_DESC d{};
        if (SUCCEEDED(adjusted.exposureResource->QueryInterface(IID_PPV_ARGS(&texture))))
        {
            texture->GetDesc(&d);
        }
        if (d.Width != 1 || d.Height != 1 || d.MipLevels != 1 || d.ArraySize != 1 ||
            d.SampleDesc.Count != 1)
        {
            adjusted.exposureResource = nullptr;
        }
    }
    hr = native.modelBridge->Process(
        adjusted, desc.Width, desc.Height, ViewFormat(desc.Format),
        [&](ID3D12GraphicsCommandList* list, const Frame12& frame, std::shared_ptr<void>& keep,
            ID3D12Resource*& output, D3D12_RESOURCE_STATES& state)
        {
            auto payload = std::static_pointer_cast<Payload>(keep);
            if (!payload)
            {
                payload = std::make_shared<Payload>();
            }
            keep = payload;
            auto f = frame;
            if (estimateMotion)
            {
                const auto flowResult =
                    native.modelFlow->Record(list, frame, payload->flow, options);
                if (FAILED(flowResult))
                {
                    return flowResult;
                }
                flowRecorded = true;
                // uvMotion is UV-space at frame resolution, so the pixel conversion moves to
                // the consumer's MV.Scale exactly as the reference implementation does.
                f.resources[2] = native.modelFlow->MotionUV();
                f.generatedMotion = true;
                {
                    const auto motionDesc = f.resources[2]->GetDesc();
                    f.guides.motionScaleX = float(motionDesc.Width);
                    f.guides.motionScaleY = float(motionDesc.Height);
                }
            }
            if (PreviewActive())
            {
                try
                {
                    if (!payload->preview)
                    {
                        payload->preview = std::make_shared<Preview12>(device);
                    }
                    auto previewFrame = f;
                    previewFrame.guides.displayScale = DisplayScale();
                    previewFrame.guides.depthContrast = DepthContrast();
                    const auto previewHr = payload->preview->Record(list, previewFrame);
                    if (SUCCEEDED(previewHr))
                    {
                        native.modelBridge->QueuePreview(
                            payload->preview->image.Get(),
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                        previewRecorded = true;
                    }
                }
                catch (HRESULT previewError)
                {
                    if (FAILED(device->GetDeviceRemovedReason()))
                    {
                        throw previewError;
                    }
                } // Optional preview must not disable a healthy model.
            }
            payload->pass
                .reset(); // Completed slot ownership must not masquerade as new partial model work.
            auto rc = native.model->Record(list, f, input.exposure, settings.pass, reset,
                                           payload->pass, output);
            PublishPass(native.model->Snapshot());
            state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            // Flow commands or partial feature work must be submitted even on a model failure.
            if (rc != S_OK && (payload->pass || flowRecorded || previewRecorded))
            {
                output = frame.resources[0].Get();
                return S_OK;
            }
            return rc;
        });
    if (hr == S_OK && previewRecorded && native.modelBridge->PreviewImage())
    {
        native.modelPreviewLatest = true;
        native.generatedLatest = false;
        Global().status.resources[2] = {desc.Width, desc.Height, unsigned(DXGI_FORMAT_R16G16_FLOAT),
                                        true};
        Report(S_OK);
    }
    auto status = native.model->Snapshot();
    if (hr == S_OK && status.edited)
    {
        native.modelStream = input.stream;
        native.modelOrigin = input.origin;
        auto image = native.modelBridge->Image();
        ComPtr<ID3D11Resource> output;
        if (image)
        {
            image->GetResource(&output);
        }
        if (output)
        {
            context->CopyResource(writeback ? writeback : input.resources[0], output.Get());
            status.reason = "NR evaluated; result copy queued to DX11 color";
        }
        else
        {
            status.ready = false;
            status.error = E_POINTER;
            status.reason = "NR bridge output missing; original frame preserved";
        }
    }
    else if (hr == S_FALSE)
    {
        status.ready = status.edited = false;
        status.actualPasses = 0;
        status.reason = "NR GPU busy; original frame preserved";
    }
    else if (FAILED(hr))
    {
        status.ready = status.edited = false;
        status.actualPasses = 0;
        status.error = hr;
        status.reason = "NR bridge submission failed; original frame preserved";
    }
    PublishPass(status);
    if (estimate && hr != S_OK && flowRecorded)
    {
        // Recorded but not submitted: the flow pyramid and the model history are unusable.
        native.modelFlow->Reset();
        native.model->InvalidateHistory();
    }
    return hr;
}
}
bool PreviewSelected()
{
    return previewSelected.load();
}
bool PanelVisible()
{
    return panelVisible.load();
}
bool PreviewActive()
{
    return PanelVisible() && PreviewSelected();
}
void SetPanelVisible(const void* owner, bool visible)
{
    if (!owner)
    {
        return;
    }
    // Window thread. Only the membership update runs on the leaf lock; the reset below needs
    // the frame lock, so it is taken only after the leaf lock is released.
    bool wasVisible{};
    {
        std::lock_guard lock(Global().panelMutex);
        auto& r = Global();
        if (visible)
        {
            r.visiblePanels.insert(owner);
        }
        else
        {
            r.visiblePanels.erase(owner);
        }
        wasVisible = panelVisible.exchange(!r.visiblePanels.empty());
    }
    if (wasVisible && !PanelVisible())
    {
        std::lock_guard lock(Global().mutex);
        ResetPreviewHistory();
    }
}
bool NativeCaptureEnabled()
{
    return nativeCaptureEnabled.load();
}
void SetNativeCaptureEnabled(bool value)
{
    nativeCaptureEnabled = value;
}
bool DepthContrast()
{
    return depthContrast.load();
}
void SetDepthContrast(bool value, bool persist)
{
    depthContrast = value;
    if (persist)
    {
        Save(L"DepthContrast", value ? L"1" : L"0");
    }
}
bool DepthFlip()
{
    return depthFlip.load();
}
void SetDepthFlip(bool value, bool persist)
{
    if (depthFlip.exchange(value) != value)
    {
        std::lock_guard lock(Global().mutex);
        ResetPreviewHistory();
    }
    if (persist)
    {
        Save(L"DepthUpsideDown", value ? L"1" : L"0", L"Resources");
    }
}
DenseFlowSettings FlowOptions()
{
    std::lock_guard lock(Global().mutex);
    return flowOptions;
}
void SetFlowOptions(DenseFlowSettings value, bool persist)
{
    value.fov = std::isfinite(value.fov) ? std::clamp(value.fov, 1.f, 179.f) : 60.f;
    value.farPlane =
        std::isfinite(value.farPlane) ? std::clamp(value.farPlane, 2.f, 100000.f) : 1000.f;
    value.depthMultiplier = std::isfinite(value.depthMultiplier)
                                ? std::clamp(value.depthMultiplier, .001f, 1000.f)
                                : 1.f;
    auto finite = [](float v, float low, float high, float fallback)
    {
        return std::isfinite(v) ? std::clamp(v, low, high) : fallback;
    };
    value.depthScaleX = finite(value.depthScaleX, .001f, 1000.f, 1);
    value.depthScaleY = finite(value.depthScaleY, .001f, 1000.f, 1);
    value.depthOffsetX = finite(value.depthOffsetX, -1000.f, 1000.f, 0);
    value.depthOffsetY = finite(value.depthOffsetY, -1000.f, 1000.f, 0);
    value.depthPixelOffsetX = finite(value.depthPixelOffsetX, -16384.f, 16384.f, 0);
    value.depthPixelOffsetY = finite(value.depthPixelOffsetY, -16384.f, 16384.f, 0);
    {
        std::lock_guard lock(Global().mutex);
        flowOptions = value;
    }
    if (persist)
    {
        wchar_t text[64]{};
        auto save = [&](const wchar_t* key, float number)
        {
            swprintf_s(text, L"%.9g", double(number));
            Save(key, text, L"Resources");
        };
        swprintf_s(text, L"%.9g", double(value.fov));
        Save(L"FlowFov", text, L"Resources");
        save(L"FlowFarPlane", value.farPlane);
        save(L"FlowDepthMultiplier", value.depthMultiplier);
        save(L"FlowDepthScaleX", value.depthScaleX);
        save(L"FlowDepthScaleY", value.depthScaleY);
        save(L"FlowDepthOffsetX", value.depthOffsetX);
        save(L"FlowDepthOffsetY", value.depthOffsetY);
        save(L"FlowDepthPixelOffsetX", value.depthPixelOffsetX);
        save(L"FlowDepthPixelOffsetY", value.depthPixelOffsetY);
        Save(L"FlowLogDepth", value.logarithmic ? L"1" : L"0", L"Resources");
    }
}
void SetPreviewSelected(bool value)
{
    previewSelected = value;
    if (!value)
    {
        std::lock_guard lock(Global().mutex);
        ResetPreviewHistory();
    }
}
float DisplayScale()
{
    return gain.load();
}
void SetDisplayScale(float value, bool persist)
{
    if (!std::isfinite(value))
    {
        return;
    }
    value = std::clamp(value, .001f, 64.f);
    gain = value;
    if (persist)
    {
        wchar_t text[32]{};
        swprintf_s(text, L"%.6g", double(value));
        Save(L"ResourceScale", text);
    }
}
PreviewStatus PreviewSnapshot()
{
    std::lock_guard lock(Global().mutex);
    return Global().status;
}
DepthStatus DepthSnapshot()
{
    std::lock_guard lock(Global().mutex);
    return Global().depth;
}
void RetireCompleted()
{
    RetireModelFeatures();
    if (!modelCached.load() || ModelRequested())
    {
        return;
    }
    std::lock_guard lock(Global().mutex);
    if (!ModelRequested() && modelCached.exchange(false))
    {
        for (auto& [key, native] : Global().native)
        {
            (void)key;
            native.model.reset();
            native.modelFlow.reset();
            if (native.modelBridge && !Runtime().OwnsDevice(native.modelBridge->Device()))
            {
                native.modelBridge.reset();
            }
            native.modelPreviewLatest = false;
        }
    }
}
void ReleaseNativeCapture(ID3D11Device* device)
{
    std::lock_guard lock(Global().mutex);
    depth::Release(device);
    for (auto it = Global().native.begin(); it != Global().native.end();)
    {
        if (it->second.device.Get() == device)
        {
            it = Global().native.erase(it);
        }
        else
        {
            ++it;
        }
    }
    RetireModelFeatures();
}
void ReleaseGameStream11(UINT64 stream)
{
    if (!stream)
    {
        return;
    }
    std::lock_guard lock(Global().mutex);
    for (auto& [context, native] : Global().native)
    {
        if (native.modelStream != stream)
        {
            continue;
        }
        // Bridge recordings keep submitted GPU owners; the executor parks features
        // behind their real tickets. Keep the bridge and its ordered queue alive.
        native.model.reset();
        native.modelFlow.reset();
        native.modelStream = 0;
        native.modelOrigin = InputOrigin::Unknown;
        native.externallyFed = false;
        native.modelPreviewLatest = false;
        depth::SetGameGuides(context, false);
    }
    RetireModelFeatures();
}
HRESULT Capture11(ID3D11DeviceContext* context, const Input11& original,
                  const ResourceInfo* described)
{
    if (!ProcessingActive() || !Enabled())
    {
        return S_FALSE;
    }
    if (!context)
    {
        return E_POINTER;
    }
    std::lock_guard lock(Global().mutex);
    if (!ProcessingActive())
    {
        return S_FALSE;
    }
    try
    {
        auto input = original;
        input.guides.flipDepth = input.guides.flipDepth != DepthFlip();
        Info(input.resources, input.guides, described);
        auto& native = Global().native[context];
        native.externallyFed = input.resources[0] && input.resources[1] && input.resources[2];
        depth::SetGameGuides(context, native.externallyFed);
        if (ModelActive())
        {
            auto hr = Model11(native, context, input, false, described);
            if (native.modelPreviewLatest || !PreviewActive())
            {
                return hr;
            }
        }
        if (!PreviewActive())
        {
            return S_OK;
        }
        if (!native.bridge)
        {
            context->GetDevice(&native.device);
            native.bridge = std::make_unique<Bridge11>(context);
        }
        auto adjusted = input;
        adjusted.guides.displayScale = DisplayScale();
        adjusted.guides.depthContrast = DepthContrast();
        auto hr = RecordDebug(*native.bridge, adjusted);
        if (hr == S_OK)
        {
            native.generatedLatest = false;
            native.modelPreviewLatest = false;
        }
        Report(hr);
        return hr;
    }
    catch (HRESULT hr)
    {
        Report(hr);
        return hr;
    }
    catch (...)
    {
        Report(E_FAIL);
        return E_FAIL;
    }
}
void NativeFrame11(IDXGISwapChain* swap, ID3D11DeviceContext* context)
{

    if (!ProcessingActive() || !NativeCaptureEnabled() || !Enabled() || !context)
    {
        return;
    }
    std::lock_guard lock(Global().mutex);
    if (!ProcessingActive())
    {
        return;
    }
    try
    {
        auto& native = Global().native[context];
        if (!native.device)
        {
            context->GetDevice(&native.device);
        }
        if (ModelActive())
        {
            if (!native.modelBridge)
            {
                native.modelBridge = std::make_unique<Bridge11>(context);
            }
            if (FAILED(Runtime().Initialize(native.modelBridge->Device())))
            {
                depth::Release(native.device.Get());
                if (!PreviewActive())
                {
                    return;
                }
            }
        }
        ComPtr<ID3D11Texture2D> buffer;
        auto hr = swap->GetBuffer(0, IID_PPV_ARGS(&buffer));
        if (FAILED(hr))
        {
            throw hr;
        }
        D3D11_TEXTURE2D_DESC desc{};
        buffer->GetDesc(&desc);
        depth::SetGameGuides(context, native.externallyFed);
        const auto found = depth::EndFrame(context, desc.Width, desc.Height, !native.externallyFed);
        {
            auto& published = Global().depth;
            published.frameWidth = desc.Width;
            published.frameHeight = desc.Height;
            published.present = found.image != nullptr;
            published.width = found.width;
            published.height = found.height;
            published.format = found.format;
            published.candidates = found.candidates;
            published.inverted = found.inverted;
            published.deferred = found.deferred;
            published.written = found.written;
        }
        if (native.externallyFed)
        {
            native.externallyFed = false;
            return;
        } // Explicit/game NGX inputs take precedence.
        depth::Internal internal;
        if (!found.image)
        {
            if (ModelActive())
            {
                PassStatus status;
                status.reason = "Waiting for game depth; NR not executed";
                PublishPass(status);
            }
            // One frame without depth is not a discontinuity: keep the model history so the
            // next frame resumes instead of restarting temporal accumulation.
            Input11 empty;
            Info(empty.resources, empty.guides);
            Report(HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
            return;
        }
        // The swapchain backbuffer goes straight to the bridge: it copies into its shared twin
        // anyway, so a private intermediate copy here only cost one more full-frame pass.
        Input11 input{{buffer.Get(), found.image.Get(), nullptr}, {}};
        input.origin = InputOrigin::NativeCapture;
        // A scRGB/FP16 backbuffer is scene-linear and routinely exceeds 1.0, so the pass has to
        // encode it for the model instead of forwarding raw values as display domain.
        input.exposure.sceneLinear = LinearColorFormat(desc.Format);
        input.guides.depthInverted = found.inverted;
        input.guides.flipDepth = DepthFlip();
        input.guides.displayScale = DisplayScale();
        input.guides.depthContrast = DepthContrast();
        Info(input.resources, input.guides);
        if (ModelActive())
        {
            // Debug-only estimator/bridge share nothing with the model bridge; free their shared twins.
            if (native.generatedBridge)
            {
                native.generatedBridge.reset();
                native.flow.reset();
                native.generatedLatest = false;
            }
            Model11(native, context, input, true, nullptr, buffer.Get());
            return;
        }
        if (!PreviewActive())
        {
            return;
        }
        // Model-active processing returns above. This color copy is therefore
        // still the unedited game input for debug-only flow and previews.
        native.modelPreviewLatest = false;
        if (!native.generatedBridge)
        {
            native.generatedBridge = std::make_unique<Bridge11>(context);
        }
        if (!native.flow)
        {
            native.flow = std::make_unique<DenseFlow12>(native.generatedBridge->Device());
        }
        struct Payload
        {
            std::shared_ptr<void> flow;
            std::shared_ptr<Preview12> preview;
        };
        bool recorded = false;
        hr = native.generatedBridge->Process(
            input,
            desc.Width * 3, desc.Height, DXGI_FORMAT_R8G8B8A8_UNORM,
            [&](ID3D12GraphicsCommandList* list, const Frame12& frame, std::shared_ptr<void>& keep,
                ID3D12Resource*& output, D3D12_RESOURCE_STATES& outputState)
            {
                auto payload = std::static_pointer_cast<Payload>(keep);
                if (!payload)
                {
                    payload = std::make_shared<Payload>();
                    keep = payload;
                }
                const auto options = FlowOptions();
                auto result = native.flow->Record(list, frame, payload->flow, options);
                if (FAILED(result))
                {
                    return result;
                }
                recorded = true;
                auto viewFrame = frame;
                viewFrame.resources[2] = native.flow->MotionUV();
                viewFrame.generatedMotion = true;
                {
                    const auto motionDesc = viewFrame.resources[2]->GetDesc();
                    viewFrame.guides.motionScaleX = float(motionDesc.Width);
                    viewFrame.guides.motionScaleY = float(motionDesc.Height);
                }
                if (!payload->preview)
                {
                    payload->preview =
                        std::make_shared<Preview12>(native.generatedBridge->Device());
                }
                result = payload->preview->Record(list, viewFrame);
                output = payload->preview->image.Get();
                outputState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                return result;
            });
        if (hr == S_OK)
        {
            native.generatedLatest = true;
            Global().status.resources[2] = {desc.Width, desc.Height,
                                            unsigned(DXGI_FORMAT_R16G16_FLOAT), true};
        }
        // A busy slot is not a discontinuity: keep the pyramid so the next frame resumes.
        if (hr != S_OK && recorded)
        {
            native.flow->Reset();
        }
        Report(hr);
    }
    catch (HRESULT hr)
    {
        Report(hr);
    }
    catch (...)
    {
        Report(E_FAIL);
    }
}
ComPtr<ID3D11ShaderResourceView> Image11(ID3D11Device* device)
{
    if (!PreviewActive())
    {
        return {};
    }
    std::lock_guard lock(Global().mutex);
    for (auto& [context, native] : Global().native)
    {
        (void)context;
        if (native.device.Get() == device && Global().status.state != State::Failed)
        {
            if (native.modelPreviewLatest && native.modelBridge)
            {
                return native.modelBridge->PreviewImage();
            }
            if (native.generatedLatest && native.generatedBridge)
            {
                return native.generatedBridge->Image();
            }
            if (native.bridge)
            {
                return native.bridge->Image();
            }
        }
    }
    return {};
}
}

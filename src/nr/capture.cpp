// SPDX-License-Identifier: GPL-3.0-or-later
#include "capture11.hpp"
#include "ngx_parameters.hpp"
#include "guide_contract.hpp"
#include "model_runtime.hpp"
#include "model_settings.hpp"
#include "pass.hpp"
#include "../graphics.hpp"
#include <array>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <type_traits>

namespace lite::nr
{
namespace
{
std::recursive_mutex mutex;
struct Metadata : GuideMetadata
{
    UINT64 stream{};
};
struct Entry
{
    HMODULE module{};
    // The game's original NGX functions: create, evaluate, release, evaluate_C. Written
    // once under mutex by ResolveNgx; the stubs read them lock-free on every call.
    std::atomic<FARPROC> hot[4]{};
    std::map<void*, Metadata> features;
};
std::array<Entry, 8> entries;
GameDlssStatus gameDlss;
size_t lastEntry{};
void* lastFeature{};
UINT64 streamGeneration{};
thread_local bool evaluating{};

Metadata Describe(const Parameters* parameters)
{
    Metadata metadata;
    int flags{};
    metadata.flagsKnown = parameters->Get("DLSS.Feature.Create.Flags", &flags) == 1;
    metadata.flags = unsigned(flags);
    metadata.renderW = Read<unsigned>(parameters, "Width");
    metadata.renderH = Read<unsigned>(parameters, "Height");
    metadata.outW = Read<unsigned>(parameters, "OutWidth");
    metadata.outH = Read<unsigned>(parameters, "OutHeight");
    return metadata;
}

Guides Describe(const Parameters* parameters, const Metadata& metadata,
                ID3D11Resource* const* resources, ResourceInfo (&info)[3])
{
    Guides guides;
    guides.motionScaleX = Read<float>(parameters, "MV.Scale.X", 1);
    guides.motionScaleY = Read<float>(parameters, "MV.Scale.Y", 1);
    guides.depthInverted = metadata.flagsKnown && (metadata.flags & 8) != 0;
    guides.originX[0] = Read<unsigned>(parameters, "DLSS.Output.Subrect.Base.X");
    guides.originY[0] = Read<unsigned>(parameters, "DLSS.Output.Subrect.Base.Y");
    guides.originX[1] = Read<unsigned>(parameters, "DLSS.Input.Depth.Subrect.Base.X");
    guides.originY[1] = Read<unsigned>(parameters, "DLSS.Input.Depth.Subrect.Base.Y");
    guides.originX[2] = Read<unsigned>(parameters, "DLSS.Input.MV.Subrect.Base.X");
    guides.originY[2] = Read<unsigned>(parameters, "DLSS.Input.MV.Subrect.Base.Y");
    const auto width = Read<unsigned>(parameters, "DLSS.Render.Subrect.Dimensions.Width",
                                      metadata.renderW);
    const auto height = Read<unsigned>(parameters, "DLSS.Render.Subrect.Dimensions.Height",
                                       metadata.renderH);
    std::array<TextureExtent, 3> sizes{};
    for (unsigned n = 0; n < 3; ++n)
    {
        info[n] = {};
        if (!resources[n])
        {
            continue;
        }
        ComPtr<ID3D11Texture2D> texture;
        if (SUCCEEDED(resources[n]->QueryInterface(IID_PPV_ARGS(&texture))))
        {
            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
            sizes[n] = {desc.Width, desc.Height};
            info[n] = {desc.Width, desc.Height, unsigned(desc.Format), true};
        }
    }
    if (FAILED(ResolveGuideExtents(guides, metadata, sizes, width, height)))
    {
        throw E_INVALIDARG;
    }
    return guides;
}

Exposure DescribeExposure(const Parameters* parameters, const Metadata& metadata,
                          DXGI_FORMAT format)
{
    Exposure exposure;
    exposure.sceneLinear = metadata.flagsKnown && (metadata.flags & 1) &&
                           LinearColorFormat(format);
    exposure.prePresent = parameters->Get("DLSS.Pre.Exposure", &exposure.pre) == 1;
    exposure.exposurePresent = parameters->Get("DLSS.Exposure.Scale", &exposure.scale) == 1;
    return exposure;
}

template <size_t N>
unsigned __cdecl Create11(ID3D11DeviceContext* context, int feature, Parameters* parameters,
                          void** output)
{
    using Fn = unsigned(__cdecl*)(ID3D11DeviceContext*, int, Parameters*, void**);
    const auto address = entries[N].hot[0].load(std::memory_order_acquire);
    if (!address)
    {
        return 0xBAD0000Bu;
    }
    Metadata metadata;
    bool described{};
    if (feature == 1 && parameters)
    {
        try
        {
            // The original create may reuse these keys for optimal-settings queries.
            // Preserve the caller's creation contract before entering that call.
            metadata = Describe(parameters);
            described = true;
        }
        catch (...)
        {
        }
    }
    const auto result = reinterpret_cast<Fn>(address)(context, feature, parameters, output);
    if (result == 1 && described && output && *output)
    {
        try
        {
            std::lock_guard lock(mutex);
            metadata.stream = ++streamGeneration;
            entries[N].features[*output] = metadata;
        }
        catch (...)
        {
        }
    }
    return result;
}

template <size_t N, bool C = false>
unsigned __cdecl Evaluate11(ID3D11DeviceContext* context, void* handle,
                            std::conditional_t<C, const Parameters*, Parameters*> parameters,
                            void* callback)
{
    using ParameterPointer = std::conditional_t<C, const Parameters*, Parameters*>;
    using Fn = unsigned(__cdecl*)(ID3D11DeviceContext*, void*, ParameterPointer, void*);
    const auto address = entries[N].hot[C ? 3 : 1].load(std::memory_order_acquire);
    if (!address)
    {
        return 0xBAD0000Bu;
    }
    if (evaluating)
    {
        return reinterpret_cast<Fn>(address)(context, handle, parameters, callback);
    }
    struct Guard
    {
        Guard()
        {
            evaluating = true;
        }
        ~Guard()
        {
            evaluating = false;
        }
    } guard;
    const auto result = reinterpret_cast<Fn>(address)(context, handle, parameters, callback);
    if (parameters && context)
    {
        Metadata metadata;
        bool found{};
        {
            std::lock_guard lock(mutex);
            const auto it = entries[N].features.find(handle);
            if (it != entries[N].features.end())
            {
                metadata = it->second;
                found = true;
                gameDlss.observed = true;
                gameDlss.released = false;
                gameDlss.result = result;
                gameDlss.lastEvaluation = GetTickCount64();
                lastEntry = N;
                lastFeature = handle;
            }
        }
        if (found && result == 1 && ProcessingActive())
        {
            try
            {
                Input11 input{{Read<ID3D11Resource*>(parameters, "Output"),
                               Read<ID3D11Resource*>(parameters, "Depth"),
                               Read<ID3D11Resource*>(parameters, "MotionVectors")},
                              {}};
                input.origin = InputOrigin::GameDlss;
                input.stream = metadata.stream;
                ResourceInfo described[3]{};
                input.guides = Describe(parameters, metadata, input.resources, described);
                if (described[0].present)
                {
                    input.exposure =
                        DescribeExposure(parameters, metadata, DXGI_FORMAT(described[0].format));
                }
                input.exposureResource = Read<ID3D11Resource*>(parameters, "ExposureTexture");
                input.reset = Read<unsigned>(parameters, "Reset") != 0;
                Capture11(context, input, described);
            }
            catch (...)
            {
            }
        }
    }
    return result;
}

template <size_t N>
unsigned __cdecl Release11(void* handle)
{
    using Fn = unsigned(__cdecl*)(void*);
    const auto address = entries[N].hot[2].load(std::memory_order_acquire);
    UINT64 stream{};
    {
        std::lock_guard lock(mutex);
        if (const auto it = entries[N].features.find(handle); it != entries[N].features.end())
        {
            stream = it->second.stream;
        }
        entries[N].features.erase(handle);
        if (gameDlss.observed && lastEntry == N && lastFeature == handle)
        {
            gameDlss.released = true;
        }
    }
    ReleaseGameStream11(stream);
    return address ? reinterpret_cast<Fn>(address)(handle) : 1;
}

struct Thunks
{
    FARPROC create, evaluate, release, evaluateC;
};
template <size_t... N>
auto Build(std::index_sequence<N...>)
{
    return std::array<Thunks, sizeof...(N)>{
        Thunks{reinterpret_cast<FARPROC>(&Create11<N>),
               reinterpret_cast<FARPROC>(&Evaluate11<N>),
               reinterpret_cast<FARPROC>(&Release11<N>),
               reinterpret_cast<FARPROC>(&Evaluate11<N, true>)}...};
}
}

GameDlssStatus GameDlssSnapshot()
{
    std::lock_guard lock(mutex);
    return gameDlss;
}

FARPROC ResolveNgx(HMODULE module, LPCSTR name, FARPROC original)
{
    if (!original || ngxLookupBypass || reinterpret_cast<ULONG_PTR>(name) <= 0xffff)
    {
        return original;
    }
    static const char* names[]{"NVSDK_NGX_D3D11_CreateFeature",
                               "NVSDK_NGX_D3D11_EvaluateFeature",
                               "NVSDK_NGX_D3D11_ReleaseFeature",
                               "NVSDK_NGX_D3D11_EvaluateFeature_C"};
    unsigned field = 4;
    for (unsigned n = 0; n < 4; ++n)
    {
        if (strcmp(name, names[n]) == 0)
        {
            field = n;
            break;
        }
    }
    if (field == 4)
    {
        return original;
    }
    static const auto thunks = Build(std::make_index_sequence<8>{});
    std::lock_guard lock(mutex);
    for (size_t n = 0; n < entries.size(); ++n)
    {
        auto& entry = entries[n];
        if (entry.module && entry.module != module)
        {
            continue;
        }
        if (!entry.module)
        {
            HMODULE pinned{};
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                        GET_MODULE_HANDLE_EX_FLAG_PIN,
                                    reinterpret_cast<LPCWSTR>(original), &pinned))
            {
                return original;
            }
            entry.module = module;
        }
        entry.hot[field].store(original, std::memory_order_release);
        const FARPROC replacements[]{thunks[n].create, thunks[n].evaluate, thunks[n].release, thunks[n].evaluateC};
        return replacements[field];
    }
    return original;
}
}

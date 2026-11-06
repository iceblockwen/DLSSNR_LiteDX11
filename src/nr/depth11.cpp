// SPDX-License-Identifier: GPL-3.0-or-later
#include "depth11.hpp"
#include "../graphics.hpp"
#include <map>
#include <mutex>
#include <vector>
#include <algorithm>
#include <atomic>
namespace lite::nr::depth
{
namespace
{
struct Candidate
{
    ComPtr<ID3D11Texture2D> source;
    bool inverted{}, deferred{};
    uint64_t lastSeen{};
    unsigned width{}, height{}, format{};
};
struct Context
{
    std::shared_ptr<std::atomic<bool>> tracking = std::make_shared<std::atomic<bool>>(true);
    ID3D11Device* device{}; // identity only, never dereferenced
    // Held, not borrowed: an unheld depth view could be released and its address reused, so
    // the identity compare below needs the interface to stay alive.
    ComPtr<ID3D11DepthStencilView> boundView;
    ComPtr<ID3D11Resource> bound;
    // Held, not borrowed: a released state object's address could be reused by another state.
    ComPtr<ID3D11DepthStencilState> lastState; // parsed once per distinct state object
    bool writes = true, deferred{}, inverted{};
    IUnknown* lifetime{}; // identity only; the native context owns the cookie
    std::map<ID3D11Resource*, Candidate> candidates;
    Candidate* current{};   // map nodes are stable; invalidate before erase/clear
    bool currentResolved{}; // includes unsupported/null bindings
    // Binding scan: last candidate bound while depth writes were enabled this frame.
    Candidate* lastWrite{};
    uint64_t lastWriteFrame{};
    unsigned frameWidth{}, frameHeight{}; // last EndFrame size, used to shape-filter bindings
    uint64_t frame{};
};
struct Registry
{
    std::recursive_mutex mutex;
    std::map<ID3D11DeviceContext*, Context> contexts;
    std::atomic<UINT64> policyEpoch{1};
    ComPtr<ID3D11Texture2D> pinned, lastWinner;
};
Registry& R()
{
    static auto* r = new Registry;
    return *r;
}
constexpr GUID CookieId{0x709db8b3, 0xd101, 0x49e1, {0xa0, 0x50, 0x88, 0xa1, 0xef, 0x63, 0xb7, 0x4a}};
constexpr GUID ContextCookieId{0x321f6971, 0x74c9, 0x4adc, {0x8d, 0xca, 0x11, 0x0a, 0x51, 0x4b, 0x96, 0x90}};
class ContextLifetime final : public IUnknown
{
    std::atomic<ULONG> refs{1};
    ID3D11DeviceContext* context;

  public:
    explicit ContextLifetime(ID3D11DeviceContext* c) : context(c)
    {
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID i, void** o) override
    {
        if (!o)
        {
            return E_POINTER;
        }
        *o = nullptr;
        if (i != IID_IUnknown)
        {
            return E_NOINTERFACE;
        }
        *o = this;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++refs;
    }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const auto n = --refs;
        if (!n)
        {
            std::lock_guard lock(R().mutex);
            auto it = R().contexts.find(context);
            if (it != R().contexts.end() && it->second.lifetime == this)
            {
                R().contexts.erase(it);
                ++R().policyEpoch;
            }
            delete this;
        }
        return n;
    }
};
class Recording final : public IUnknown
{
    std::atomic<ULONG> refs{1};

  public:
    std::vector<Candidate> candidates;
    // The deferred context's last write-enabled binding; the immediate context adopts it on execute.
    ComPtr<ID3D11Texture2D> written;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID i, void** o) override
    {
        if (!o)
        {
            return E_POINTER;
        }
        *o = nullptr;
        if (i != IID_IUnknown)
        {
            return E_NOINTERFACE;
        }
        *o = this;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++refs;
    }
    ULONG STDMETHODCALLTYPE Release() override
    {
        auto n = --refs;
        if (!n)
        {
            delete this;
        }
        return n;
    }
};
DXGI_FORMAT Family(DXGI_FORMAT f)
{
    switch (f)
    {
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_TYPELESS:
            return DXGI_FORMAT_R32_TYPELESS;
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24G8_TYPELESS:
            return DXGI_FORMAT_R24G8_TYPELESS;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
            return DXGI_FORMAT_R32G8X24_TYPELESS;
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_R16_TYPELESS:
            return DXGI_FORMAT_R16_TYPELESS;
        default:
            return DXGI_FORMAT_UNKNOWN;
    }
}
Candidate* Find(Context& c, ID3D11Resource* resource)
{
    if (!resource)
    {
        return nullptr;
    }
    auto it = c.candidates.find(resource);
    if (it != c.candidates.end())
    {
        return &it->second;
    }
    ComPtr<ID3D11Texture2D> t;
    if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&t))))
    {
        return nullptr;
    }
    D3D11_TEXTURE2D_DESC d{};
    t->GetDesc(&d);
    if (d.SampleDesc.Count != 1 || d.MipLevels != 1 || d.ArraySize != 1 || Family(d.Format) == DXGI_FORMAT_UNKNOWN)
    {
        return nullptr;
    }
    // Bound memory: only candidates with observed work survive; evict the weakest.
    if (c.candidates.size() >= 32)
    {
        auto weakest = std::min_element(c.candidates.begin(), c.candidates.end(), [](auto& a, auto& b)
                                        {
                                            return a.second.lastSeen < b.second.lastSeen;
                                        });
        if (c.current == &weakest->second)
        {
            c.current = nullptr;
            c.currentResolved = false;
        }
        if (c.lastWrite == &weakest->second)
        {
            c.lastWrite = nullptr;
        }
        c.candidates.erase(weakest);
    }
    auto& candidate = c.candidates[resource];
    candidate.source = t;
    candidate.deferred = c.deferred;
    candidate.lastSeen = c.frame;
    candidate.width = d.Width;
    candidate.height = d.Height;
    candidate.format = unsigned(d.Format);
    return &candidate;
}
bool Shape(unsigned, unsigned, unsigned, unsigned);
bool Shape(unsigned w, unsigned h, unsigned fw, unsigned fh)
{
    if (!w || !h || !fw || !fh)
    {
        return false;
    }
    const double ratio = double(w) * fh / (double(h) * fw);
    // A candidate larger than the frame is legal (a 2x supersampled scene depth is the real
    // one); size only ranks, it never excludes. Written-vs-test-only decides first.
    return ratio > .95 && ratio < 1.05 && w >= fw / 2 && h >= fh / 2 && w <= fw * 2 && h <= fh * 2;
}
}
static Context& RegisterLocked(ID3D11DeviceContext* context)
{
    auto existing = R().contexts.find(context);
    if (existing != R().contexts.end())
    {
        return existing->second;
    }
    auto& c = R().contexts[context];
    ComPtr<ID3D11Device> d;
    context->GetDevice(&d);
    c.device = d.Get();
    ++R().policyEpoch;
    c.deferred = context->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED;
    ComPtr<ID3D11DepthStencilView> view;
    context->OMGetRenderTargets(0, nullptr, &view);
    if (view)
    {
        view->GetResource(&c.bound);
    }
    c.boundView = view;
    c.current = Find(c, c.bound.Get());
    c.currentResolved = true;
    ComPtr<ID3D11DepthStencilState> state;
    context->OMGetDepthStencilState(&state, nullptr);
    if (state)
    {
        D3D11_DEPTH_STENCIL_DESC desc{};
        state->GetDesc(&desc);
        c.writes = desc.DepthEnable && desc.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL;
        c.inverted = desc.DepthFunc == D3D11_COMPARISON_GREATER || desc.DepthFunc == D3D11_COMPARISON_GREATER_EQUAL;
    }
    auto* lifetime = new ContextLifetime(context);
    c.lifetime = lifetime;
    const auto hr = context->SetPrivateDataInterface(ContextCookieId, lifetime);
    lifetime->Release();
    if (FAILED(hr))
    {
        R().contexts.erase(context);
        ++R().policyEpoch;
        throw hr;
    }
    return c;
}
void Register(ID3D11DeviceContext* context)
{
    if (!context)
    {
        return;
    }
    std::lock_guard lock(R().mutex);
    RegisterLocked(context);
}
// Per-thread context lookup. Map nodes are stable and every registry mutation bumps
// policyEpoch, so a cached pointer stays valid until the epoch changes.
Context& ContextLocked(ID3D11DeviceContext* context)
{
    struct Cache
    {
        ID3D11DeviceContext* context{};
        UINT64 epoch{};
        Context* value{};
    };
    thread_local Cache cache;
    const auto epoch = R().policyEpoch.load(std::memory_order_relaxed);
    if (cache.context == context && cache.epoch == epoch && cache.value)
    {
        return *cache.value;
    }
    auto& c = RegisterLocked(context);
    cache.context = context;
    cache.value = &c;
    cache.epoch = R().policyEpoch.load(std::memory_order_relaxed);
    return c;
}
// The same depth target is rebound for every draw of a pass; identity comparison keeps that
// case off the COM path entirely.
void BindView(Context& c, ID3D11DepthStencilView* view)
{
    if (view == c.boundView.Get() && c.currentResolved)
    {
        return;
    }
    c.boundView = view;
    ComPtr<ID3D11Resource> next;
    if (view)
    {
        view->GetResource(&next);
    }
    c.bound = next;
    c.current = Find(c, c.bound.Get());
    c.currentResolved = true;
}
// Binding scan: the last depth bound while writes were enabled and shaped like the scene is
// the selection. Both conditions are stable within a frame, so only the first one counts.
void MarkWrite(Context& c)
{
    if (!c.current || !c.writes)
    {
        return;
    }
    if (c.lastWrite == c.current && c.lastWriteFrame == c.frame)
    {
        return;
    }
    if (c.frameWidth && !Shape(c.current->width, c.current->height, c.frameWidth, c.frameHeight))
    {
        return;
    }
    c.lastWrite = c.current;
    c.lastWriteFrame = c.frame;
}
bool TrackingEnabled(ID3D11DeviceContext* context)
{
    struct Cache
    {
        ID3D11DeviceContext* context{};
        UINT64 epoch{};
        std::shared_ptr<std::atomic<bool>> policy;
    };
    thread_local Cache cache;
    const auto epoch = R().policyEpoch.load();
    if (cache.context != context || cache.epoch != epoch || !cache.policy)
    {
        std::lock_guard lock(R().mutex);
        cache.policy = RegisterLocked(context).tracking;
        cache.context = context;
        cache.epoch = R().policyEpoch.load();
    }
    return cache.policy->load();
}
void SetGameGuides(ID3D11DeviceContext* context, bool supplied)
{
    if (!context)
    {
        return;
    }
    std::lock_guard lock(R().mutex);
    auto it = R().contexts.find(context);
    if (it == R().contexts.end())
    {
        if (!supplied)
        {
            return;
        }
        RegisterLocked(context);
        it = R().contexts.find(context);
    }
    auto& c = it->second;
    const bool enabled = !supplied;
    if (c.tracking->load() == enabled)
    {
        return;
    }
    c.current = nullptr;
    c.currentResolved = false;
    c.candidates.clear();
    c.bound.Reset();
    c.boundView.Reset();
    if (enabled)
    {
        // Binds/state may have changed while hooks bypassed tracking. Re-snapshot
        // the native context before allowing the first fallback draw.
        ComPtr<ID3D11DepthStencilView> view;
        context->OMGetRenderTargets(0, nullptr, &view);
        if (view)
        {
            view->GetResource(&c.bound);
        }
        ComPtr<ID3D11DepthStencilState> state;
        context->OMGetDepthStencilState(&state, nullptr);
        c.writes = true;
        c.inverted = false;
        c.lastState = state.Get();
        if (state)
        {
            D3D11_DEPTH_STENCIL_DESC desc{};
            state->GetDesc(&desc);
            c.writes = desc.DepthEnable && desc.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL;
            c.inverted = desc.DepthFunc == D3D11_COMPARISON_GREATER || desc.DepthFunc == D3D11_COMPARISON_GREATER_EQUAL;
        }
    }
    c.tracking->store(enabled);
}
void Binding(ID3D11DeviceContext* context, ID3D11DepthStencilView* view)
{
    std::lock_guard lock(R().mutex);
    auto& c = ContextLocked(context);
    BindView(c, view);
    if (c.current)
    {
        c.current->lastSeen = c.frame;
    }
    MarkWrite(c);
}
void Cleared(ID3D11DeviceContext* context, ID3D11DepthStencilView* view, UINT flags, float value)
{
    if (!view || !(flags & D3D11_CLEAR_DEPTH))
    {
        return;
    }
    std::lock_guard lock(R().mutex);
    auto& c = ContextLocked(context);
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    if (auto* candidate = Find(c, resource.Get()))
    {
        candidate->lastSeen = c.frame;
        if (value == 0 || value == 1)
        {
            candidate->inverted = value == 0;
        }
    }
}
void State(ID3D11DeviceContext* context, ID3D11DepthStencilState* state)
{
    std::lock_guard lock(R().mutex);
    auto& c = ContextLocked(context);
    // Games reuse a handful of depth-stencil states; only parse a state object once.
    if (state != c.lastState.Get())
    {
        c.lastState = state;
        c.writes = true;
        c.inverted = false;
        if (state)
        {
            D3D11_DEPTH_STENCIL_DESC d{};
            state->GetDesc(&d);
            c.writes = d.DepthEnable && d.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL;
            c.inverted = d.DepthFunc == D3D11_COMPARISON_GREATER || d.DepthFunc == D3D11_COMPARISON_GREATER_EQUAL;
        }
    }
    // Binding scan: a state change that enables depth writes marks the bound depth.
    MarkWrite(c);
}
void ClearState(ID3D11DeviceContext* context)
{
    std::lock_guard lock(R().mutex);
    auto it = R().contexts.find(context);
    if (it != R().contexts.end())
    {
        it->second.current = nullptr;
        it->second.currentResolved = true;
        it->second.bound.Reset();
        it->second.boundView.Reset();
        it->second.writes = true;
        it->second.inverted = false;
        it->second.lastState.Reset();
    }
}
void Finished(ID3D11DeviceContext* context, ID3D11CommandList* list, BOOL restore)
{
    std::lock_guard lock(R().mutex);
    auto it = R().contexts.find(context);
    if (it == R().contexts.end())
    {
        return;
    }
    auto* recording = new Recording;
    for (auto& [key, c] : it->second.candidates)
    {
        recording->candidates.push_back(c);
    }
    if (it->second.lastWrite)
    {
        recording->written = it->second.lastWrite->source;
    }
    if (list)
    {
        list->SetPrivateDataInterface(CookieId, recording);
    }
    recording->Release();
    it->second.current = nullptr;
    it->second.currentResolved = false;
    // Both point into the map that is cleared below.
    it->second.lastWrite = nullptr;
    it->second.lastWriteFrame = 0;
    it->second.candidates.clear();
    if (!restore)
    {
        ClearState(context);
    }
}
void Executed(ID3D11DeviceContext* context, ID3D11CommandList* list, BOOL restore)
{
    std::lock_guard lock(R().mutex);
    auto& state = RegisterLocked(context);
    Recording* recording = nullptr;
    UINT bytes = sizeof(recording);
    if (list && SUCCEEDED(list->GetPrivateData(CookieId, &bytes, &recording)) && recording)
    {
        for (auto& candidate : recording->candidates)
        {
            if (state.candidates.size() >= 32 && !state.candidates.contains(candidate.source.Get()))
            {
                continue;
            }
            auto& target = state.candidates[candidate.source.Get()];
            target.source = candidate.source;
            target.inverted = candidate.inverted;
            target.deferred = candidate.deferred;
            target.width = candidate.width;
            target.height = candidate.height;
            target.format = candidate.format;
            target.lastSeen = state.frame;
            // Command lists run in submission order, so the last executed one decides.
            if (recording->written && candidate.source.Get() == recording->written.Get())
            {
                state.lastWrite = &target;
                state.lastWriteFrame = state.frame;
            }
        }
        recording->Release();
    }
    if (!restore)
    {
        ClearState(context);
    }
}
Result EndFrame(ID3D11DeviceContext* context, unsigned width, unsigned height, bool capture)
{
    std::lock_guard lock(R().mutex);
    Result result;
    auto it = R().contexts.find(context);
    if (it == R().contexts.end())
    {
        return result;
    }
    auto& state = it->second;
    Candidate* best = nullptr;
    auto& reg = R();
    // A candidate accepted while the frame size was still unknown must not stay selected.
    if (state.lastWrite && state.frameWidth &&
        !Shape(state.lastWrite->width, state.lastWrite->height, state.frameWidth, state.frameHeight))
    {
        state.lastWrite = nullptr;
    }
    if (reg.pinned)
    {
        for (auto& [key, c] : state.candidates)
        {
            if (c.source.Get() == reg.pinned.Get())
            {
                best = &c;
                break;
            }
        }
    }
    // The selection is sticky, so a frame that binds nothing still keeps the last scene depth.
    if (!best && capture && state.lastWrite)
    {
        best = state.lastWrite;
    }
    if (best)
    {
        result.image = best->source;
        reg.lastWinner = best->source;
        result.inverted = best->inverted;
        result.deferred = best->deferred;
        result.written = state.lastWrite == best && state.lastWriteFrame == state.frame;
        result.width = best->width;
        result.height = best->height;
        result.format = best->format;
        result.candidates = unsigned(state.candidates.size());
    }
    state.frameWidth = width;
    state.frameHeight = height;
    ++state.frame;
    for (auto candidate = state.candidates.begin(); candidate != state.candidates.end();)
    {
        if (state.frame - candidate->second.lastSeen > 3)
        {
            if (state.current == &candidate->second)
            {
                state.current = nullptr;
                state.currentResolved = false;
            }
            if (state.lastWrite == &candidate->second)
            {
                state.lastWrite = nullptr;
            }
            if (state.bound.Get() == candidate->first)
            {
                state.bound.Reset();
                state.boundView.Reset();
                state.currentResolved = false;
            }
            candidate = state.candidates.erase(candidate);
        }
        else
        {
            ++candidate;
        }
    }
    return result;
}
bool PinCurrent()
{
    std::lock_guard lock(R().mutex);
    if (!R().lastWinner)
    {
        return false;
    }
    R().pinned = R().lastWinner;
    return true;
}
void ClearPin()
{
    std::lock_guard lock(R().mutex);
    R().pinned.Reset();
}
bool Pinned()
{
    std::lock_guard lock(R().mutex);
    return R().pinned != nullptr;
}
void Release(ID3D11Device* device)
{
    std::lock_guard lock(R().mutex);
    bool erased{};
    for (auto it = R().contexts.begin(); it != R().contexts.end();)
    {
        if (it->second.device == device)
        {
            it = R().contexts.erase(it);
            erased = true;
        }
        else
        {
            ++it;
        }
    }
    R().pinned.Reset();
    R().lastWinner.Reset();
    if (erased)
    {
        ++R().policyEpoch;
    }
}
}

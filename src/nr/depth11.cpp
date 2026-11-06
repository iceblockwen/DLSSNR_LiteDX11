// SPDX-License-Identifier: GPL-3.0-or-later
#include "depth11.hpp"
#include "../graphics.hpp"
#include <array>
#include <map>
#include <mutex>
#include <shared_mutex>
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
struct Context;
class ContextLifetime;
struct Context
{
    // One lock per context: recording threads never serialize on each other's state.
    std::mutex mutex;
    // Cleared when the registry entry is retired, so a thread-local cache stops using it.
    std::atomic<bool> alive{true};
    std::shared_ptr<std::atomic<bool>> tracking = std::make_shared<std::atomic<bool>>(true);
    ID3D11Device* device{}; // identity only, never dereferenced
    // Held, not borrowed: an unheld depth view could be released and its address reused, so
    // the identity compare below needs the interface to stay alive.
    ComPtr<ID3D11DepthStencilView> boundView;
    ComPtr<ID3D11Resource> bound;
    // Held, not borrowed: a released state object's address could be reused by another state.
    ComPtr<ID3D11DepthStencilState> lastState; // parsed once per distinct state object
    bool writes = true, deferred{}, inverted{};
    // Allocated while the registry lock is held, published outside it; the native
    // context owns the only reference afterwards.
    ContextLifetime* lifetime{};
    std::map<ID3D11Resource*, Candidate> candidates;
    Candidate* current{};   // map nodes are stable; invalidate before erase/clear
    bool currentResolved{}; // includes unsupported/null bindings
    // Binding scan: last candidate bound while depth writes were enabled this frame.
    Candidate* lastWrite{};
    uint64_t lastWriteFrame{};
    unsigned frameWidth{}, frameHeight{}; // last EndFrame size, used to shape-filter bindings
    uint64_t frame{};
    // A rejected texture is re-queried periodically, not for every draw that binds it.
    struct Rejected
    {
        ID3D11Resource* resource{};
        uint64_t frame{};
    };
    std::array<Rejected, 16> rejected{};
    unsigned rejectedNext{};
    static constexpr uint64_t RejectedRetryFrames = 4;
};
struct Registry
{
    // Shared for lookups: the per-context lock, not this one, guards the bind state.
    std::shared_mutex mutex;
    std::map<ID3D11DeviceContext*, std::shared_ptr<Context>> contexts;
    ComPtr<ID3D11Texture2D> pinned, lastWinner; // mutex-guarded, never touched under a context lock
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
            {
                // Never called with the registry lock held: the cookie is attached
                // and replaced outside it, so this lock is always a fresh one.
                std::unique_lock lock(R().mutex);
                auto it = R().contexts.find(context);
                if (it != R().contexts.end() && it->second->lifetime == this)
                {
                    it->second->alive.store(false, std::memory_order_relaxed);
                    R().contexts.erase(it);
                }
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
    auto reject = [&]
    {
        auto& entry = c.rejected[c.rejectedNext];
        entry = {resource, c.frame};
        c.rejectedNext = (c.rejectedNext + 1) % unsigned(c.rejected.size());
        return nullptr;
    };
    for (const auto& entry : c.rejected)
    {
        // Texture addresses are recycled, so a rejection expires instead of being permanent.
        if (entry.resource == resource && c.frame - entry.frame <= Context::RejectedRetryFrames)
        {
            return nullptr;
        }
    }
    ComPtr<ID3D11Texture2D> t;
    if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&t))))
    {
        return reject();
    }
    D3D11_TEXTURE2D_DESC d{};
    t->GetDesc(&d);
    if (d.SampleDesc.Count != 1 || d.MipLevels != 1 || d.ArraySize != 1 || Family(d.Format) == DXGI_FORMAT_UNKNOWN)
    {
        return reject();
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
// Registry unique lock held; the context is not published yet, so its own lock is not needed.
static std::shared_ptr<Context> RegisterLocked(ID3D11DeviceContext* context, bool& created)
{
    created = false;
    auto existing = R().contexts.find(context);
    if (existing != R().contexts.end())
    {
        return existing->second;
    }
    auto c = std::make_shared<Context>();
    ComPtr<ID3D11Device> d;
    context->GetDevice(&d);
    c->device = d.Get();
    c->deferred = context->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED;
    ComPtr<ID3D11DepthStencilView> view;
    context->OMGetRenderTargets(0, nullptr, &view);
    if (view)
    {
        view->GetResource(&c->bound);
    }
    c->boundView = view;
    c->current = Find(*c, c->bound.Get());
    c->currentResolved = true;
    ComPtr<ID3D11DepthStencilState> state;
    context->OMGetDepthStencilState(&state, nullptr);
    if (state)
    {
        D3D11_DEPTH_STENCIL_DESC desc{};
        state->GetDesc(&desc);
        c->writes = desc.DepthEnable && desc.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL;
        c->inverted = desc.DepthFunc == D3D11_COMPARISON_GREATER || desc.DepthFunc == D3D11_COMPARISON_GREATER_EQUAL;
    }
    // Allocated under the lock so a concurrent acquire sees the entry as taken.
    c->lifetime = new ContextLifetime(context);
    created = true;
    return R().contexts.emplace(context, std::move(c)).first->second;
}
// Registry lock NOT held. Replacing a previous cookie releases it, which re-enters
// the registry through ContextLifetime::Release, so this can never run under the lock.
bool AttachCookie(ID3D11DeviceContext* context, const std::shared_ptr<Context>& owner)
{
    auto* lifetime = owner->lifetime;
    if (FAILED(context->SetPrivateDataInterface(ContextCookieId, lifetime)))
    {
        owner->lifetime = nullptr;
        // The context may have taken a reference before failing, so release ours
        // instead of assuming this is the only one.
        lifetime->Release();
        std::unique_lock lock(R().mutex);
        owner->alive.store(false, std::memory_order_relaxed);
        auto it = R().contexts.find(context);
        if (it != R().contexts.end() && it->second == owner)
        {
            R().contexts.erase(it);
        }
        return false;
    }
    lifetime->Release(); // the native context now owns the only reference
    return true;
}
// Lookup only takes the registry lock in shared mode; the returned owner keeps the
// context alive even if another thread retires it in the meantime.
std::shared_ptr<Context> AcquireSlow(ID3D11DeviceContext* context)
{
    {
        std::shared_lock lock(R().mutex);
        auto it = R().contexts.find(context);
        if (it != R().contexts.end())
        {
            return it->second;
        }
    }
    std::shared_ptr<Context> owner;
    bool created{};
    {
        std::unique_lock lock(R().mutex);
        owner = RegisterLocked(context, created);
    }
    if (created)
    {
        AttachCookie(context, owner);
    }
    return owner;
}
// Recording threads bind and clear thousands of times per frame, so the lookup result
// is cached per thread: the hot path is two atomics instead of a shared lock and a map
// probe. A retired context expires the weak reference.
std::shared_ptr<Context> Acquire(ID3D11DeviceContext* context)
{
    struct Cache
    {
        ID3D11DeviceContext* key{};
        std::weak_ptr<Context> value;
    };
    thread_local Cache cache;
    if (cache.key == context)
    {
        if (auto owner = cache.value.lock(); owner && owner->alive.load(std::memory_order_relaxed))
        {
            return owner;
        }
    }
    auto owner = AcquireSlow(context);
    cache.key = context;
    cache.value = owner;
    return owner;
}
void Register(ID3D11DeviceContext* context)
{
    if (!context)
    {
        return;
    }
    std::shared_ptr<Context> owner;
    bool created{};
    {
        std::unique_lock lock(R().mutex);
        owner = RegisterLocked(context, created);
    }
    if (created)
    {
        AttachCookie(context, owner);
    }
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
    // The flag is queried for every observed call, so a thread-local owner keeps the
    // per-draw path to two atomic reads instead of a registry lookup.
    struct Cache
    {
        ID3D11DeviceContext* context{};
        std::weak_ptr<Context> owner;
        std::shared_ptr<std::atomic<bool>> policy;
    };
    thread_local Cache cache;
    if (cache.context != context || cache.owner.expired())
    {
        auto owner = Acquire(context);
        if (!owner)
        {
            return false;
        }
        cache.context = context;
        cache.policy = owner->tracking;
        cache.owner = owner;
    }
    return cache.policy->load();
}
void SetGameGuides(ID3D11DeviceContext* context, bool supplied)
{
    if (!context)
    {
        return;
    }
    auto owner = Acquire(context);
    if (!owner)
    {
        return;
    }
    auto& c = *owner;
    std::lock_guard lock(c.mutex);
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
    auto owner = Acquire(context);
    if (!owner)
    {
        return;
    }
    auto& c = *owner;
    std::lock_guard lock(c.mutex);
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
    auto owner = Acquire(context);
    if (!owner)
    {
        return;
    }
    auto& c = *owner;
    std::lock_guard lock(c.mutex);
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
    auto owner = Acquire(context);
    if (!owner)
    {
        return;
    }
    auto& c = *owner;
    std::lock_guard lock(c.mutex);
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
// Context lock already held by the caller.
void ClearStateLocked(Context& state)
{
    state.current = nullptr;
    state.currentResolved = true;
    state.bound.Reset();
    state.boundView.Reset();
    state.writes = true;
    state.inverted = false;
    state.lastState.Reset();
}
void ClearState(ID3D11DeviceContext* context)
{
    auto owner = Acquire(context);
    if (!owner)
    {
        return;
    }
    std::lock_guard lock(owner->mutex);
    ClearStateLocked(*owner);
}
void Finished(ID3D11DeviceContext* context, ID3D11CommandList* list, BOOL restore)
{
    auto owner = Acquire(context);
    if (!owner)
    {
        return;
    }
    auto& state = *owner;
    std::lock_guard lock(state.mutex);
    auto* recording = new Recording;
    for (auto& [key, c] : state.candidates)
    {
        recording->candidates.push_back(c);
    }
    if (state.lastWrite)
    {
        recording->written = state.lastWrite->source;
    }
    if (list)
    {
        list->SetPrivateDataInterface(CookieId, recording);
    }
    recording->Release();
    state.current = nullptr;
    state.currentResolved = false;
    // Both point into the map that is cleared below.
    state.lastWrite = nullptr;
    state.lastWriteFrame = 0;
    state.candidates.clear();
    // A deferred context never advances the frame counter, so rejection stamps are
    // aged out at the command list boundary instead.
    state.rejected = {};
    state.rejectedNext = 0;
    if (!restore)
    {
        ClearStateLocked(state);
    }
}
void Executed(ID3D11DeviceContext* context, ID3D11CommandList* list, BOOL restore)
{
    auto owner = Acquire(context);
    if (!owner)
    {
        return;
    }
    auto& state = *owner;
    std::lock_guard lock(state.mutex);
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
        ClearStateLocked(state);
    }
}
Result EndFrame(ID3D11DeviceContext* context, unsigned width, unsigned height, bool capture)
{
    Result result;
    auto owner = Acquire(context);
    if (!owner)
    {
        return result;
    }
    auto& state = *owner;
    // Registry lock first, context lock second: the order no path reverses.
    std::lock_guard registry(R().mutex);
    std::lock_guard lock(state.mutex);
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
    std::unique_lock lock(R().mutex);
    if (!R().lastWinner)
    {
        return false;
    }
    R().pinned = R().lastWinner;
    return true;
}
void ClearPin()
{
    std::unique_lock lock(R().mutex);
    R().pinned.Reset();
}
bool Pinned()
{
    std::shared_lock lock(R().mutex);
    return R().pinned != nullptr;
}
void Release(ID3D11Device* device)
{
    std::unique_lock lock(R().mutex);
    for (auto it = R().contexts.begin(); it != R().contexts.end();)
    {
        if (it->second->device == device)
        {
            it->second->alive.store(false, std::memory_order_relaxed);
            it = R().contexts.erase(it);
        }
        else
        {
            ++it;
        }
    }
    R().pinned.Reset();
    R().lastWinner.Reset();
}
}

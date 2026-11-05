// SPDX-License-Identifier: GPL-3.0-or-later
#include "depth11.hpp"
#include "capture11.hpp"
#include "model_settings.hpp"
#include "../detour_hook.hpp"
#include <mutex>
namespace lite::nr::depth
{
namespace
{
using lite::detour::Hook;
std::recursive_mutex hookMutex;
bool Active()
{
    return !internal && NativeCaptureEnabled() && ProcessingActive();
}
template <class F>
void Observe(ID3D11DeviceContext* context, F&& f) noexcept
{
    if (!Active())
    {
        return;
    }
    try
    {
        if (TrackingEnabled(context))
        {
            f();
        }
    }
    catch (...)
    {
    }
}
struct Bind
{
    template <class F>
    static void Call(F fn, ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rt, ID3D11DepthStencilView* d)
    {
        Observe(c, [&]
                {
                    Binding(c, d);
                });
        fn(c, n, rt, d);
    }
};
struct BindUav
{
    template <class F>
    static void Call(F fn, ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rt, ID3D11DepthStencilView* d, UINT start, UINT count, ID3D11UnorderedAccessView* const* u, UINT const* initial)
    {
        if (n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
        {
            Observe(c, [&]
                    {
                        Binding(c, d);
                    });
        }
        fn(c, n, rt, d, start, count, u, initial);
    }
};
struct Clear
{
    template <class F>
    static void Call(F fn, ID3D11DeviceContext* c, ID3D11DepthStencilView* d, UINT flags, FLOAT z, UINT8 s)
    {
        Observe(c, [&]
                {
                    Cleared(c, d, flags, z);
                });
        fn(c, d, flags, z, s);
    }
};
struct ClearView
{
    template <class F>
    static void Call(F fn, ID3D11DeviceContext1* c, ID3D11View* v, const FLOAT* values, const D3D11_RECT* rects, UINT count)
    {
        Observe(c, [&]
                {
                    ComPtr<ID3D11DepthStencilView> d;
                    if (v && values && SUCCEEDED(v->QueryInterface(IID_PPV_ARGS(&d))))
                    {
                        Cleared(c, d.Get(), D3D11_CLEAR_DEPTH, values[0]);
                    }
                });
        fn(c, v, values, rects, count);
    }
};
struct StateChange
{
    template <class F>
    static void Call(F fn, ID3D11DeviceContext* c, ID3D11DepthStencilState* d, UINT s)
    {
        fn(c, d, s);
        Observe(c, [&]
                {
                    State(c, d);
                });
    }
};
struct ClearAll
{
    template <class F>
    static void Call(F fn, ID3D11DeviceContext* c)
    {
        fn(c);
        Observe(c, [&]
                {
                    ClearState(c);
                });
    }
};
struct Finish
{
    template <class F>
    static HRESULT Call(F fn, ID3D11DeviceContext* c, BOOL restore, ID3D11CommandList** list)
    {
        const auto hr = fn(c, restore, list);
        if (SUCCEEDED(hr))
        {
            Observe(c, [&]
                    {
                        Finished(c, list ? *list : nullptr, restore);
                    });
        }
        return hr;
    }
};
struct Execute
{
    template <class F>
    static void Call(F fn, ID3D11DeviceContext* c, ID3D11CommandList* list, BOOL restore)
    {
        fn(c, list, restore);
        Observe(c, [&]
                {
                    Executed(c, list, restore);
                });
    }
};
using HBind = Hook<Bind, void, ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*>;
using HBindUav = Hook<BindUav, void, ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*, UINT, UINT, ID3D11UnorderedAccessView* const*, UINT const*>;
using HClear = Hook<Clear, void, ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8>;
using HClearView = Hook<ClearView, void, ID3D11DeviceContext1*, ID3D11View*, const FLOAT*, const D3D11_RECT*, UINT>;
using HState = Hook<StateChange, void, ID3D11DeviceContext*, ID3D11DepthStencilState*, UINT>;
using HAll = Hook<ClearAll, void, ID3D11DeviceContext*>;
using HFinish = Hook<Finish, HRESULT, ID3D11DeviceContext*, BOOL, ID3D11CommandList**>;
using HExecute = Hook<Execute, void, ID3D11DeviceContext*, ID3D11CommandList*, BOOL>;
void End(bool success)
{
    HBind::Finish(success);
    HBindUav::Finish(success);
    HClear::Finish(success);
    HClearView::Finish(success);
    HState::Finish(success);
    HAll::Finish(success);
    HFinish::Finish(success);
    HExecute::Finish(success);
}
void Stage(ID3D11DeviceContext* c)
{
    auto** v = *reinterpret_cast<void***>(c);
    HBind::Stage(v[33]);
    HBindUav::Stage(v[34]);
    HClear::Stage(v[53]);
    HState::Stage(v[36]);
    HAll::Stage(v[110]);
    HFinish::Stage(v[114]);
    HExecute::Stage(v[58]);
    ComPtr<ID3D11DeviceContext1> c1;
    if (SUCCEEDED(c->QueryInterface(IID_PPV_ARGS(&c1))))
    {
        HClearView::Stage((*reinterpret_cast<void***>(c1.Get()))[132]);
    }
}
}
void Install(ID3D11Device* d, ID3D11DeviceContext* c)
{
    Register(c);
    std::lock_guard lock(hookMutex);
    // Probe both native method sets; no CreateDeferredContext detour with an incorrect ABI.
    ComPtr<ID3D11DeviceContext> deferred;
    Internal guard;
    const auto deferredResult = d->CreateDeferredContext(0, &deferred);
    lite::detour::Transaction<&End> transaction;
    Stage(c);
    if (SUCCEEDED(deferredResult))
    {
        Stage(deferred.Get());
    }
    transaction.Commit();
}
}

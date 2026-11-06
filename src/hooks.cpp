#include "wrappers.hpp"
#include "runtime.hpp"
#include "detour_hook.hpp"
#include "factory_origin.hpp"
#include "nr/capture11.hpp"
#include <atomic>
#include <mutex>

namespace
{
using lite::detour::Hook;
using lite::detour::Method;
std::recursive_mutex hookMutex;
thread_local unsigned captureDepth{};
thread_local unsigned parentBypass{};
struct Scope
{
    Scope()
    {
        ++captureDepth;
    }
    ~Scope()
    {
        --captureDepth;
    }
};
// Lite/Opt: install related creation hooks in one transaction, check Commit,
// abort incomplete groups. No native Present/ResizeBuffers/Release detours.
struct Create
{
    template <class Fn>
    static HRESULT Call(Fn fn, IDXGIFactory* self, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out)
    {
        lite::capture::NativeFactoryScope internal;
        const auto hr = fn(self, device, desc, out);
        if (SUCCEEDED(hr))
        {
            lite::capture::WrapSwapchain(device, out);
        }
        return hr;
    }
};
struct CreateHwnd
{
    template <class Fn>
    static HRESULT Call(Fn fn, IDXGIFactory2* self, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGIOutput* output, IDXGISwapChain1** out)
    {
        lite::capture::NativeFactoryScope internal;
        const auto hr = fn(self, device, hwnd, desc, fullscreen, output, out);
        if (SUCCEEDED(hr))
        {
            lite::capture::WrapSwapchain(device, out);
        }
        return hr;
    }
};
struct CreateCore
{
    template <class Fn>
    static HRESULT Call(Fn fn, IDXGIFactory2* self, IUnknown* device, IUnknown* window, const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* output, IDXGISwapChain1** out)
    {
        lite::capture::NativeFactoryScope internal;
        const auto hr = fn(self, device, window, desc, output, out);
        if (SUCCEEDED(hr))
        {
            lite::capture::WrapSwapchain(device, out);
        }
        return hr;
    }
};
struct CreateComposition
{
    template <class Fn>
    static HRESULT Call(Fn fn, IDXGIFactory2* self, IUnknown* device, const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* output, IDXGISwapChain1** out)
    {
        lite::capture::NativeFactoryScope internal;
        const auto hr = fn(self, device, desc, output, out);
        if (SUCCEEDED(hr))
        {
            lite::capture::WrapSwapchain(device, out);
        }
        return hr;
    }
};
struct Create11
{
    template <class Fn>
    static HRESULT Call(Fn fn, IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE software, UINT flags, const D3D_FEATURE_LEVEL* levels, UINT count, UINT sdk, const DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swap, ID3D11Device** device, D3D_FEATURE_LEVEL* level, ID3D11DeviceContext** context)
    {
        lite::capture::NativeFactoryScope internal;
        const auto hr = fn(adapter, type, software, flags, levels, count, sdk, desc, swap, device, level, context);
        if (SUCCEEDED(hr))
        {
            lite::capture::WrapSwapchain(device ? *device : nullptr, swap);
        }
        return hr;
    }
};
struct Device11
{
    template <class Fn>
    static HRESULT Call(Fn fn, IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE software, UINT flags,
                        const D3D_FEATURE_LEVEL* levels, UINT count, UINT sdk, ID3D11Device** device, D3D_FEATURE_LEVEL* level, ID3D11DeviceContext** context)
    {
        lite::capture::NativeFactoryScope internal;
        const auto hr = fn(adapter, type, software, flags, levels, count, sdk, device, level, context);
        if (SUCCEEDED(hr))
        {
            ComPtr<ID3D11Device> created;
            ComPtr<IDXGIDevice> dxgi;
            ComPtr<IDXGIAdapter> actual;
            if (device && *device)
            {
                created = *device;
            }
            else if (context && *context)
            {
                (*context)->GetDevice(&created);
            }
            if (created && SUCCEEDED(created.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&actual)))
            {
                lite::capture::CaptureAdapter(actual.Get());
            }
        }
        return hr;
    }
};
using HDevice11 = Hook<Device11, HRESULT, IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**>;
using HCreate = Hook<Create, HRESULT, IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**>;
using HHwnd = Hook<CreateHwnd, HRESULT, IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**>;
using HCore = Hook<CreateCore, HRESULT, IDXGIFactory2*, IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**>;
using HComposition = Hook<CreateComposition, HRESULT, IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**>;
using HCreate11 = Hook<Create11, HRESULT, IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**>;
struct FactoryExport
{
    template <class Fn>
    static HRESULT CallFrom(Fn fn, void* caller, REFIID iid, void** out)
    {
        auto hr = fn(iid, out);
        if (SUCCEEDED(hr) && !lite::NativeFactoryCaller(caller))
        {
            lite::AttachFactory(iid, out);
        }
        return hr;
    }
};
struct FactoryExport2
{
    template <class Fn>
    static HRESULT CallFrom(Fn fn, void* caller, UINT flags, REFIID iid, void** out)
    {
        auto hr = fn(flags, iid, out);
        if (SUCCEEDED(hr) && !lite::NativeFactoryCaller(caller))
        {
            lite::AttachFactory(iid, out);
        }
        return hr;
    }
};
struct Parent
{
    template <class Fn>
    static HRESULT Call(Fn fn, IDXGIObject* self, REFIID iid, void** out)
    {
        const auto hr = fn(self, iid, out);
        if (SUCCEEDED(hr) && !parentBypass)
        {
            lite::AttachFactory(iid, out);
        }
        return hr;
    }
};
using HParent = Hook<Parent, HRESULT, IDXGIObject*, REFIID, void**>;
using HFactory = Hook<FactoryExport, HRESULT, REFIID, void**>;
using HFactory2 = Hook<FactoryExport2, HRESULT, UINT, REFIID, void**>;
thread_local unsigned loaderDepth{};
void Watch(HMODULE module)
{
    if (!module || (reinterpret_cast<ULONG_PTR>(module) & 3))
    {
        return;
    }
    wchar_t path[1024]{};
    if (!GetModuleFileNameW(module, path, 1024))
    {
        return;
    }
    const auto leaf = wcsrchr(path, L'\\');
    if (_wcsicmp(leaf ? leaf + 1 : path, L"d3d11.dll") == 0)
    {
        try
        {
            lite::InstallD3D11Capture(module);
        }
        catch (HRESULT)
        {
        }
        catch (...)
        {
        }
    }
}
struct LoaderScope
{
    LoaderScope()
    {
        ++loaderDepth;
    }
    ~LoaderScope()
    {
        --loaderDepth;
    }
};
struct Load
{
    template <class Fn, class Name>
    static HMODULE Call(Fn fn, Name name)
    {
        if (loaderDepth)
        {
            return fn(name);
        }
        LoaderScope guard;
        auto module = fn(name);
        Watch(module);
        return module;
    }
};
struct LoadEx
{
    template <class Fn, class Name>
    static HMODULE Call(Fn fn, Name name, HANDLE file, DWORD flags)
    {
        if (loaderDepth)
        {
            return fn(name, file, flags);
        }
        LoaderScope guard;
        auto module = fn(name, file, flags);
        if (!(flags & (LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE | DONT_RESOLVE_DLL_REFERENCES)))
        {
            Watch(module);
        }
        return module;
    }
};
using HLoadA = Hook<Load, HMODULE, LPCSTR>;
using HLoadW = Hook<Load, HMODULE, LPCWSTR>;
using HLoadExA = Hook<LoadEx, HMODULE, LPCSTR, HANDLE, DWORD>;
using HLoadExW = Hook<LoadEx, HMODULE, LPCWSTR, HANDLE, DWORD>;
struct ProcLookup
{
    template <class Fn>
    static FARPROC Call(Fn fn, HMODULE module, LPCSTR name)
    {
        const auto original = fn(module, name);
        return lite::nr::ResolveNgx(module, name, original);
    }
};
using HProcLookup = Hook<ProcLookup, FARPROC, HMODULE, LPCSTR>;
void Finish(bool success)
{
    HDevice11::Finish(success);
    HParent::Finish(success);
    HCreate::Finish(success);
    HHwnd::Finish(success);
    HCore::Finish(success);
    HComposition::Finish(success);
    HCreate11::Finish(success);
    HFactory::Finish(success);
    HFactory2::Finish(success);
    HLoadA::Finish(success);
    HLoadW::Finish(success);
    HLoadExA::Finish(success);
    HLoadExW::Finish(success);
    HProcLookup::Finish(success);
}
using Transaction = lite::detour::Transaction<&Finish>;
void Pin()
{
    static bool pinned = false;
    if (pinned)
    {
        return;
    }
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&Pin), &module))
    {
        throw HRESULT_FROM_WIN32(GetLastError());
    }
    pinned = true;
}
// Per-frame/per-enumeration installs: the vtable entry is almost always the one
// already detoured, so skip the lock and the Detours transaction entirely.
template <class H>
void InstallOnce(std::atomic<void*>& last, void* target)
{
    if (!target || last.load(std::memory_order_acquire) == target)
    {
        return;
    }
    std::lock_guard lock(hookMutex);
    if (!H::Installed(target))
    {
        Pin();
        Transaction transaction;
        H::Stage(target);
        transaction.Commit();
    }
    last.store(target, std::memory_order_release);
}
std::atomic<void*> lastParent{};
}
namespace lite::capture
{
NativeFactoryScope::NativeFactoryScope()
{
    ++parentBypass;
}
NativeFactoryScope::~NativeFactoryScope()
{
    --parentBypass;
}
// DX11 reaches a factory through IDXGIAdapter::GetParent as well, so the adapter
// found by D3D11CreateDevice is where this chain starts; it is live for this backend.
void CaptureAdapter(IDXGIAdapter* adapter) noexcept
{
    if (!adapter)
    {
        return;
    }
    try
    {
        InstallOnce<HParent>(lastParent, Method(adapter, 6));
    }
    catch (HRESULT)
    {
    }
    catch (...)
    {
    }
}
void WrapSwapchainObject(IUnknown* device, IDXGISwapChain** output) noexcept
{
    if (!Usable())
    {
        return;
    }
    if (!output || !*output || captureDepth >= 4)
    {
        return;
    }
    ComPtr<ID3D11Device> device11;
    if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&device11))))
    {
        return;
    }
    Scope scope;
    try
    {
        ComPtr<IUnknown> ours;
        if (SUCCEEDED((*output)->QueryInterface(SwapchainIID, &ours)))
        {
            return;
        }
        DXGI_SWAP_CHAIN_DESC desc{};
        HRESULT hr;
        {
            NativeFactoryScope internal;
            hr = (*output)->GetDesc(&desc);
        }
        // GTA creates an auxiliary small chain (180x44); wrapping it breaks the game.
        if (SUCCEEDED(hr) && (desc.BufferDesc.Width < 100 || desc.BufferDesc.Height < 100))
        {
            return;
        }
        ComPtr<IDXGISwapChain4> native;
        if (FAILED((*output)->QueryInterface(IID_PPV_ARGS(&native))))
        {
            return;
        }
        auto* wrapper = new Swapchain(native.Get());
        (*output)->Release();
        *output = wrapper;
        TrackSwapchain(wrapper, device);
    }
    catch (HRESULT)
    {
    }
    catch (...)
    {
    }
}
}
namespace lite
{
void InstallD3D11Capture(HMODULE module)
{
    std::lock_guard lock(hookMutex);
    const auto fn = GetProcAddress(module, "D3D11CreateDeviceAndSwapChain");
    const auto single = GetProcAddress(module, "D3D11CreateDevice");
    if (!fn || !single)
    {
        throw HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    }
    if (HCreate11::Installed(reinterpret_cast<void*>(fn)) && HDevice11::Installed(reinterpret_cast<void*>(single)))
    {
        return;
    }
    Pin();
    Transaction transaction;
    HCreate11::Stage(reinterpret_cast<void*>(fn));
    HDevice11::Stage(reinterpret_cast<void*>(single));
    transaction.Commit();
}
void InstallCapture()
{
    wchar_t path[MAX_PATH]{};
    if (!GetSystemDirectoryW(path, MAX_PATH) || wcscat_s(path, L"\\dxgi.dll"))
    {
        throw E_FAIL;
    }
    const auto dxgi = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dxgi)
    {
        throw HRESULT_FROM_WIN32(GetLastError());
    }
    const auto kernel = GetModuleHandleW(L"kernel32.dll");
    {
        std::lock_guard lock(hookMutex);
        Pin();
        Transaction transaction;
        HFactory::Stage(reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory")));
        HFactory::Stage(reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")));
        HFactory2::Stage(reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory2")));
        HLoadA::Stage(reinterpret_cast<void*>(GetProcAddress(kernel, "LoadLibraryA")));
        HLoadW::Stage(reinterpret_cast<void*>(GetProcAddress(kernel, "LoadLibraryW")));
        HLoadExA::Stage(reinterpret_cast<void*>(GetProcAddress(kernel, "LoadLibraryExA")));
        HLoadExW::Stage(reinterpret_cast<void*>(GetProcAddress(kernel, "LoadLibraryExW")));
        HProcLookup::Stage(reinterpret_cast<void*>(GetProcAddress(kernel, "GetProcAddress")));
        transaction.Commit();
    }
    // Lite EnsureFactoryWatches: pre-existing factories share these native method entries.
    using CreateFn = HRESULT(WINAPI*)(REFIID, void**);
    ComPtr<IDXGIFactory> probe;
    const auto create = reinterpret_cast<CreateFn>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
    const auto hr = create(IID_PPV_ARGS(&probe));
    if (FAILED(hr))
    {
        throw hr;
    }
    if (const auto module = GetModuleHandleW(L"d3d11.dll"))
    {
        InstallD3D11Capture(module);
    }
}
void AttachFactory(REFIID, void** output) noexcept
{
    if (!Usable())
    {
        return;
    }
    // NativeFactoryScope protects native DXGI/D3D internals, including factory
    // exports re-entered by the creation call, not only adapter GetParent.
    if (parentBypass)
    {
        return;
    }
    if (!output || !*output || captureDepth >= 4)
    {
        return;
    }
    Scope scope;
    try
    {
        // The game keeps the native factory object. Only its CreateSwapChain*
        // entries are detoured, and each of those wraps the chain it returns.
        // Handing the game a foreign IDXGIFactory7 breaks its own DXGI calls
        // (Alan Wake 2 loses the device with 0x887A002B), so nothing is swapped.
        auto* object = static_cast<IUnknown*>(*output);
        ComPtr<IDXGIFactory> factory;
        if (FAILED(object->QueryInterface(IID_PPV_ARGS(&factory))))
        {
            return;
        }
        {
            std::lock_guard lock(hookMutex);
            const auto create = Method(factory.Get(), 10);
            ComPtr<IDXGIFactory2> two;
            factory.As(&two);
            const auto hwnd = two ? Method(two.Get(), 15) : nullptr;
            const auto core = two ? Method(two.Get(), 16) : nullptr;
            const auto composition = two ? Method(two.Get(), 24) : nullptr;
            const auto d3d11 = GetModuleHandleW(L"d3d11.dll");
            const auto combined = d3d11 ? reinterpret_cast<void*>(GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain")) : nullptr;
            if (!HCreate::Installed(create) || (hwnd && !HHwnd::Installed(hwnd)) ||
                (core && !HCore::Installed(core)) || (composition && !HComposition::Installed(composition)) ||
                (combined && !HCreate11::Installed(combined)))
            {
                Pin();
                Transaction transaction;
                HCreate::Stage(create);
                if (two)
                {
                    HHwnd::Stage(hwnd);
                    HCore::Stage(core);
                    HComposition::Stage(composition);
                }
                if (combined)
                {
                    HCreate11::Stage(combined);
                }
                transaction.Commit();
            }
        }
    }
    catch (HRESULT)
    {
    }
    catch (...)
    {
    }
}
}

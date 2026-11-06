// The swapchain side of the panel, split out of panel.cpp: it owns the registry and
// creates and retires one panel per swapchain. No NR and no UI work happens here.
#include "graphics_registry.hpp"
#include <d3d11.h>
namespace lite
{
using Microsoft::WRL::ComPtr;
// Process-lifetime registry avoids GPU work during DLL destruction under loader lock.
GraphicsRegistry& Graphics()
{
    static auto* registry = new GraphicsRegistry;
    return *registry;
}
std::recursive_mutex& GraphicsMutex()
{
    return Graphics().mutex;
}
void TrackSwapchain(IDXGISwapChain* swap, IUnknown* device) noexcept
{
    try
    {
        ComPtr<ID3D11Device> device11;
        if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&device11))))
        {
            return;
        }
        std::lock_guard lock(Graphics().mutex);
        if (Graphics().panels.contains(swap))
        {
            return;
        }
        auto panel = std::make_shared<Panel>(swap, device11.Get());
        Graphics().panels.emplace(swap, std::move(panel));
    }
    catch (HRESULT)
    {
    }
    catch (...)
    {
    }
}
void DrawSwapchain(IDXGISwapChain* swap, UINT flags) noexcept
{
    if (flags & DXGI_PRESENT_TEST)
    {
        return;
    }
    try
    {
        // The panel is kept alive by the reference; the NR chain then runs without the
        // registry lock, so window messages never wait for a frame's worth of GPU work.
        std::shared_ptr<Panel> panel;
        {
            std::lock_guard lock(Graphics().mutex);
            const auto it = Graphics().panels.find(swap);
            if (it == Graphics().panels.end())
            {
                return;
            }
            panel = it->second;
        }
        panel->ProcessFrame();
        std::lock_guard lock(Graphics().mutex);
        panel->DrawUi();
    }
    catch (...)
    {
    }
}
HRESULT ResizeSwapchain(IDXGISwapChain* swap, bool before, HRESULT result) noexcept
{
    try
    {
        std::lock_guard lock(Graphics().mutex);
        const auto it = Graphics().panels.find(swap);
        if (it == Graphics().panels.end())
        {
            return S_OK;
        }
        if (before)
        {
            it->second->BeforeResize();
        }
        else
        {
            it->second->AfterResize(result);
        }
        return S_OK;
    }
    catch (HRESULT hr)
    {
        FailSwapchain(swap, hr);
        return hr;
    }
    catch (...)
    {
        FailSwapchain(swap, E_FAIL);
        return E_FAIL;
    }
}
void FailSwapchain(IDXGISwapChain* swap, HRESULT error) noexcept
{
    try
    {
        std::lock_guard lock(Graphics().mutex);
        const auto it = Graphics().panels.find(swap);
        if (it != Graphics().panels.end())
        {
            it->second->Fail(error);
        }
    }
    catch (...)
    {
    }
}
void ReleaseSwapchain(IUnknown* swap) noexcept
{
    try
    {
        std::lock_guard lock(Graphics().mutex);
        const auto it = Graphics().panels.find(reinterpret_cast<IDXGISwapChain*>(swap));
        if (it != Graphics().panels.end())
        {
            auto panel = std::move(it->second);
            Graphics().panels.erase(it);
            panel->Close();
        }
    }
    catch (...)
    {
    }
}
}

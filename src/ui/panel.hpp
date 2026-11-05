#pragma once
#include "../graphics.hpp"
#include <d3d11.h>
#include <wrl/client.h>
#include <memory>
#include <mutex>

namespace lite
{
class Panel
{
  public:
    Panel(IDXGISwapChain* swapchain, ID3D11Device* device);
    ~Panel();
    void Draw(UINT presentFlags) noexcept;
    void BeforeResize();
    void AfterResize(HRESULT result);
    void Close();
    void Fail(HRESULT error) noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
void TrackSwapchain(IDXGISwapChain* swap, IUnknown* device) noexcept;
void DrawSwapchain(IDXGISwapChain* swap, UINT flags) noexcept;
HRESULT ResizeSwapchain(IDXGISwapChain* swap, bool before, HRESULT result) noexcept;
void ReleaseSwapchain(IUnknown* swap) noexcept;
void FailSwapchain(IDXGISwapChain* swap, HRESULT error) noexcept;
std::recursive_mutex& GraphicsMutex();
}

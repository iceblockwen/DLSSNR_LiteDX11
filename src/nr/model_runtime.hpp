// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ngx_parameters.hpp"
#include "contracts.hpp"
#include "pass.hpp"
#include "../runtime.hpp"
#include <mutex>
#include <atomic>
namespace lite::nr
{
// Lite's own NGX symbol lookups must reach the model directly, not the game
// NGX capture thunks installed through the global GetProcAddress hook.
inline thread_local unsigned ngxLookupBypass{};
struct NgxLookupBypass
{
    NgxLookupBypass()
    {
        ++ngxLookupBypass;
    }
    ~NgxLookupBypass()
    {
        --ngxLookupBypass;
    }
};
struct ModelStatus
{
    State state = State::Waiting;
    HRESULT error = S_OK;
    unsigned ngxResult{}, vendor{};
    wchar_t adapter[128]{}, corePath[1024]{}, modelPath[1024]{}, reason[192]{L"Model not initialized"};
    wchar_t coreVersion[64]{}, modelVersion[64]{};
};
// Lite ngx.cpp registry/module discovery and renderer.cpp core/model ABI.
class ModelRuntime
{
  public:
    explicit ModelRuntime(std::wstring directory);
    HRESULT Discover();
    HRESULT Initialize(ID3D12Device*);
    ModelStatus Snapshot() const;
    bool Available() const noexcept
    {
        return !unavailable.load();
    }
    void RetryFailedInitialization();
    bool OwnsDevice(ID3D12Device*) const;
    Parameters* Capability() const
    {
        return parameters;
    }
    unsigned Create(ID3D12GraphicsCommandList*, void**);
    unsigned Evaluate(ID3D12GraphicsCommandList*, void*);
    unsigned Release(void*);
    void SetFloat(const char*, float);
    void SetUnsigned(const char*, unsigned);
    void SetResource(const char*, ID3D12Resource*);
    static HRESULT Adapter(ID3D12Device*, unsigned& vendor, std::wstring& name);

  private:
    mutable std::recursive_mutex mutex;
    std::wstring directory;
    ModelStatus status;
    HMODULE core{}, model{}; // pinned after validated load; shared core is never shut down
    ComPtr<ID3D12Device> device;
    Parameters* parameters{}; // app-owned populated capability map
    unsigned(__cdecl* destroyParameters)(Parameters*){};
    DriverParameterWriter writer;
    std::atomic<bool> unavailable{};
    CreateFn create{};
    EvaluateFn evaluate{};
    ReleaseFn release{};
    HRESULT Fail(HRESULT, const wchar_t*, unsigned result = 0);
};
ModelRuntime& Runtime();
void DiscoverModelRuntime();
}

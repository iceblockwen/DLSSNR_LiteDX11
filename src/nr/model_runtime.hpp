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
    wchar_t corePath[1024]{}, modelPath[1024]{}, reason[192]{L"Model not initialized"};
};
// Lite ngx.cpp registry/module discovery and renderer.cpp core/model ABI.
class ModelRuntime
{
  public:
    // One lock for a group of parameter writes: a recording sets a dozen values,
    // and each individual setter would take the recursive lock again.
    class Batch
    {
      public:
        explicit Batch(ModelRuntime& owner)
            : runtime(owner), guard(runtime.mutex)
        {
        }
        void SetFloat(const char* key, float v)
        {
            runtime.writer.Float(key, v);
        }
        void SetUnsigned(const char* key, unsigned v)
        {
            runtime.writer.Unsigned(key, v);
        }
        void SetResource(const char* key, ID3D12Resource* v)
        {
            runtime.writer.Resource(key, v);
        }

      private:
        ModelRuntime& runtime;
        std::lock_guard<std::recursive_mutex> guard;
    };
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
    static HRESULT Adapter(ID3D12Device*, unsigned& vendor);

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

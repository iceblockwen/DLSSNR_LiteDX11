// SPDX-License-Identifier: GPL-3.0-or-later
// ABI surface extracted from Lite ngx.hpp. No NVIDIA binary is distributed.
#pragma once
#include "frame.hpp"
namespace lite::nr
{
struct Parameters
{
    virtual void Set(const char*, unsigned long long) = 0;
    virtual void Set(const char*, float) = 0;
    virtual void Set(const char*, double) = 0;
    virtual void Set(const char*, unsigned) = 0;
    virtual void Set(const char*, int) = 0;
    virtual void Set(const char*, ID3D11Resource*) = 0;
    virtual void Set(const char*, ID3D12Resource*) = 0;
    virtual void Set(const char*, void*) = 0;
    virtual unsigned Get(const char*, unsigned long long*) const = 0;
    virtual unsigned Get(const char*, float*) const = 0;
    virtual unsigned Get(const char*, double*) const = 0;
    virtual unsigned Get(const char*, unsigned*) const = 0;
    virtual unsigned Get(const char*, int*) const = 0;
    virtual unsigned Get(const char*, ID3D11Resource**) const = 0;
    virtual unsigned Get(const char*, ID3D12Resource**) const = 0;
    virtual unsigned Get(const char*, void**) const = 0;
    virtual void Reset() = 0;
};
// Lite render_internal.hpp U/R/F + DiscoverFloatSlot. NGX-provided capability
// maps use this ABI writer.
class DriverParameterWriter
{
  public:
    void Bind(Parameters* input)
    {
        if (p == input)
        {
            return;
        }
        p = input;
        slot = -1;
    }
    void Discover()
    {
        if (!p || slot >= 0)
        {
            return;
        }
        for (int candidate = 0; candidate < 8; ++candidate)
        {
            const float probe = .3125f;
            float back{};
            reinterpret_cast<FloatFn>(Table()[candidate])(p, "DLSSNR.Probe", probe);
            if (p->Get("DLSSNR.Probe", &back) == 1 && back == probe)
            {
                slot = candidate;
                return;
            }
        }
        slot = 1; // Lite's explicitly unverified header fallback; reported by ModelStatus.
    }
    void Float(const char* key, float value)
    {
        reinterpret_cast<FloatFn>(Table()[slot < 0 ? 1 : slot])(p, key, value);
    }
    void Unsigned(const char* key, unsigned value)
    {
        reinterpret_cast<UintFn>(Table()[3])(p, key, value);
    }
    void Resource(const char* key, ID3D12Resource* value)
    {
        reinterpret_cast<UllFn>(Table()[0])(p, key, reinterpret_cast<unsigned long long>(value));
    }

  private:
    using FloatFn = void(__cdecl*)(Parameters*, const char*, float);
    using UintFn = void(__cdecl*)(Parameters*, const char*, unsigned);
    using UllFn = void(__cdecl*)(Parameters*, const char*, unsigned long long);
    void** Table() const
    {
        return *reinterpret_cast<void***>(p);
    }
    Parameters* p{};
    int slot = -1;
};
template <class T>
T Read(const Parameters* p, const char* key, T fallback = {})
{
    T v = fallback;
    return p && p->Get(key, &v) == 1 ? v : fallback;
}
}

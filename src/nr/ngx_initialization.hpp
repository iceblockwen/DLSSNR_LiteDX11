// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ngx_parameters.hpp"
namespace lite::nr
{
// These ABIs differ: core has feature-info before SDK, model has SDK before caps.
using CoreCapsFn = unsigned(__cdecl*)(Parameters**);
using CoreInitFn = unsigned(__cdecl*)(unsigned long long, const wchar_t*, ID3D12Device*, const void*, unsigned);
using ModelInitFn = unsigned(__cdecl*)(unsigned long long, const wchar_t*, ID3D12Device*, int, const void*);
inline unsigned AcquireCapability(CoreCapsFn caps, CoreInitFn init, const wchar_t* path, ID3D12Device* device, Parameters*& output)
{
    if (!caps)
    {
        return 0xBAD0000Bu;
    }
    output = nullptr;
    unsigned result = caps(&output);
    if (result == 0xBAD00007u)
    {
        if (!init)
        {
            return 0xBAD0000Bu;
        }
        result = init(0x24480451ull, path, device, nullptr, 0x15);
        if (result == 1)
        {
            result = caps(&output);
        }
    }
    return result == 1 && !output ? 0xBAD00001u : result;
}
inline unsigned InitializeModel(ModelInitFn init, const wchar_t* path, ID3D12Device* device, Parameters* caps)
{
    if (!init || !caps)
    {
        return 0xBAD0000Bu;
    }
    volatile unsigned result = init(0x24480451ull, path, device, 0x15, caps);
    return result;
}
}

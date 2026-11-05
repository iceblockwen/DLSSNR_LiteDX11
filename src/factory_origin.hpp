// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>
namespace lite {
// Native graphics internals and overlay probes must keep their factory identity.
// Checked at creation/GetParent only, never on the per-frame Present path.
inline bool NativeFactoryCaller(void* caller) noexcept {
    HMODULE module{};
    if(!caller||!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(caller),&module))return false;
    wchar_t path[1024]{};
    const auto length=GetModuleFileNameW(module,path,1024);
    if(!length||length>=1024)return false;
    const auto* slash=wcsrchr(path,L'\\');const auto* leaf=slash?slash+1:path;
    constexpr const wchar_t* names[]{L"d3d11.dll",L"d3d12.dll",L"D3D12Core.dll",L"dxgi.dll",L"RTSSHooks64.dll"};
    for(auto name:names)if(_wcsicmp(leaf,name)==0)return true;
    return false;
}
}

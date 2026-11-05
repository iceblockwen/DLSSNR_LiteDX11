// Shared by every entry DLL: dxgi.dll and the alternative names (version.dll,
// ...). Only the forwarded export table and its fallbacks differ, so loading
// and export publication live here once.
#pragma once
#include "proxy_exports.generated.hpp"
#include <windows.h>
#include <mutex>

namespace lite::proxy
{
inline constexpr DWORD kPathLength = 1024;
inline constexpr const wchar_t* kRuntimeName = L"nvngx.dll_litedx11.dll";

inline HMODULE ownModule{};

// Folder of `module` including its trailing backslash.
inline bool ModuleFolder(HMODULE module, wchar_t (&path)[kPathLength])
{
    const auto length = GetModuleFileNameW(module, path, kPathLength);
    if (!length || length >= kPathLength)
    {
        return false;
    }
    auto* slash = wcsrchr(path, L'\\');
    if (!slash)
    {
        return false;
    }
    slash[1] = 0;
    return true;
}

inline HMODULE LoadSystem(const wchar_t* name)
{
    wchar_t path[MAX_PATH]{};
    if (!GetSystemDirectoryW(path, MAX_PATH) || wcscat_s(path, L"\\") || wcscat_s(path, name))
    {
        return nullptr;
    }
    return LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
}

// The original module keeps its own folder first so another mod can stay in the
// chain as <name>-original.dll; System32 is the fallback every game expects.
inline HMODULE LoadOriginal(const wchar_t* targetName, const wchar_t* originalName)
{
    wchar_t path[kPathLength]{};
    if (ModuleFolder(ownModule, path) && !wcscat_s(path, originalName))
    {
        if (const auto module = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH))
        {
            return module;
        }
    }
    return LoadSystem(targetName);
}

// The first stub call loads the original module and publishes every export.
// call_once leaves its flag unset when the body throws, so a failed load is
// retried by the next call instead of being cached. `index` comes from the
// generated stub and is always below kProxyExportCount.
template <class Load>
FARPROC Resolve(FARPROC (&table)[kProxyExportCount], unsigned index, Load load)
{
    static std::once_flag once;
    try
    {
        std::call_once(once, [&]
                       {
                           const auto module = load();
                           if (!module || module == ownModule)
                           {
                               throw DWORD(ERROR_MOD_NOT_FOUND);
                           }
                           for (unsigned i = 0; i < kProxyExportCount; ++i)
                           {
                               InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(&table[i]),
                                                          reinterpret_cast<PVOID>(GetProcAddress(module, kProxyExportNames[i])));
                           }
                       });
    }
    catch (...)
    {
    }
    return table[index];
}

// Loads the body beside this DLL and runs its Start export.
inline DWORD StartRuntime()
{
    wchar_t path[kPathLength]{};
    if (!ModuleFolder(ownModule, path) || wcscat_s(path, kRuntimeName))
    {
        return ERROR_BAD_PATHNAME;
    }
    const auto module = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module)
    {
        return GetLastError();
    }
    using StartFn = DWORD(WINAPI*)(void*);
    const auto start = reinterpret_cast<StartFn>(GetProcAddress(module, "Start"));
    if (!start)
    {
        FreeLibrary(module);
        return ERROR_PROC_NOT_FOUND;
    }
    return start(nullptr);
}

inline DWORD WINAPI Boot(void*)
{
    return StartRuntime();
}

// DllMain body of every entry DLL. The module stays pinned for the process
// lifetime and the body starts on a worker that is never joined under loader
// lock. Static CRT/TLS keeps thread notifications enabled.
inline void Attach(HMODULE module)
{
    ownModule = module;
    HMODULE pinned{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(module), &pinned);
    if (const auto thread = CreateThread(nullptr, 0, Boot, nullptr, 0, nullptr))
    {
        CloseHandle(thread);
    }
}
}
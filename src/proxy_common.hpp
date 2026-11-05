// SPDX-License-Identifier: GPL-3.0-or-later
// Shared by every entry DLL: dxgi.dll and the alternative names (version.dll,
// ...). Only the forwarded export table differs, so the failure log and the
// body loader live here once.
#pragma once
#include <windows.h>
#include <atomic>
#include <cstdio>
namespace lite::proxy {
struct Entry {
    std::atomic<HMODULE>* runtimeModule;
    std::atomic<DWORD>* startupError;
    const wchar_t* pluginName;
    const wchar_t* iniName;
    const wchar_t* logName;
    const wchar_t* runtimeName;
    const wchar_t* forwardingNote;
};
// Called after the loader is already running free-standing, so a plain append
// keeps the reason even when the body never starts.
inline DWORD BootFailure(HMODULE own, const Entry& entry, DWORD error) {
    entry.startupError->store(error);
    wchar_t path[1024]{};
    if (!GetModuleFileNameW(own, path, 1024)) return error;
    auto slash = wcsrchr(path, L'\\');
    if (!slash) return error;
    slash[1] = 0;
    wchar_t ini[1024]{};
    wcscpy_s(ini, path);
    if (wcscat_s(ini, entry.iniName)) return error;
    if (GetPrivateProfileIntW(L"Diagnostics", L"Level", 1, ini) < 1) return error;
    if (wcscat_s(path, entry.logName)) return error;
    FILE* file{};
    if (!_wfopen_s(&file, path, L"a, ccs=UTF-8") && file) {
        fwprintf(file, L"%ls bootstrap failed: Win32 error %lu; %ls\n", entry.pluginName, error, entry.forwardingNote);
        fclose(file);
    }
    return error;
}
// Loads and starts the body beside this DLL. Never runs under loader lock: the
// caller dispatches a worker from DllMain.
inline DWORD BootRuntime(HMODULE own, const Entry& entry) {
    auto fail = [&](DWORD error) { return BootFailure(own, entry, error); };
    wchar_t path[1024]{};
    const auto length = GetModuleFileNameW(own, path, 1024);
    if (!length) return fail(GetLastError());
    if (length >= 1024) return fail(ERROR_INSUFFICIENT_BUFFER);
    auto slash = wcsrchr(path, L'\\');
    if (!slash) return fail(ERROR_BAD_PATHNAME);
    slash[1] = 0;
    if (wcscat_s(path, entry.runtimeName)) return fail(ERROR_BAD_PATHNAME);
    const auto module = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) return fail(GetLastError());
    using StartFn = DWORD(WINAPI*)(void*);
    const auto start = reinterpret_cast<StartFn>(GetProcAddress(module, "Start"));
    if (!start) { FreeLibrary(module); return fail(ERROR_PROC_NOT_FOUND); }
    const auto error = start(nullptr);
    entry.runtimeModule->store(module);
    if (error) return fail(error);
    return 0;
}
// The original module keeps its own folder first so another mod can stay in the
// chain as <name>-original.dll; System32 is the fallback every game expects.
inline HMODULE LoadOriginal(HMODULE own, const wchar_t* targetName, const wchar_t* originalName) {
    wchar_t folder[1024]{};
    const auto length = GetModuleFileNameW(own, folder, 1024);
    if (length && length < 1024) {
        if (auto* slash = wcsrchr(folder, L'\\')) {
            slash[1] = 0;
            wchar_t beside[1024]{};
            wcscpy_s(beside, folder);
            if (!wcscat_s(beside, originalName)) {
                if (const auto module = LoadLibraryExW(beside, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) return module;
            }
        }
    }
    wchar_t system[MAX_PATH]{};
    if (!GetSystemDirectoryW(system, MAX_PATH)) return nullptr;
    if (wcscat_s(system, L"\\") || wcscat_s(system, targetName)) return nullptr;
    return LoadLibraryExW(system, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
}
}

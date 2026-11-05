// SPDX-License-Identifier: GPL-3.0-or-later
// Alternative entry point, used when the game's dxgi.dll belongs to another
// mod: forward one system DLL's exports unchanged (names, indices and ordinals
// come from src/proxy/<name>.def through the generated header and stubs) and
// start the body, exactly like the DXGI proxy does.
#include "proxy_common.hpp"
#include "proxy_exports.generated.hpp"
#include <atomic>
#include <mutex>
namespace {
HMODULE ownModule{};
std::once_flag loadOnce;
std::atomic<HMODULE> runtimeModule{};
std::atomic<DWORD> startupError{};
// A missing system module breaks the game anyway; a defined zero keeps the
// proxy from faulting inside an unrelated export.
DWORD WINAPI Unavailable() { return 0; }
const lite::proxy::Entry kEntry{&runtimeModule, &startupError, L"DLSSNR_LiteDX11",
    L"DLSSNR_LiteDX11.ini", L"DLSSNR_LiteDX11.log", L"nvngx.dll_litedx11.dll",
    L"forwarding through the original module remains available."};
DWORD WINAPI Boot(void*) { return lite::proxy::BootRuntime(ownModule, kEntry); }
}
extern "C" {
alignas(8) FARPROC ProxyTargets[kProxyExportCount]{};
FARPROC ResolveProxy(unsigned index) {
    // call_once leaves the flag unset when the body throws, so a failed load is
    // retried by the next caller.
    try {
        std::call_once(loadOnce, [] {
            const auto module = lite::proxy::LoadOriginal(ownModule, kProxyTargetName, kProxyOriginalName);
            if (!module) throw DWORD(GetLastError());
            if (module == ownModule) throw DWORD(ERROR_INVALID_HANDLE);
            for (unsigned i = 0; i < kProxyExportCount; ++i)
                InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(&ProxyTargets[i]),
                    reinterpret_cast<PVOID>(GetProcAddress(module, kProxyExportNames[i])));
        });
    } catch (DWORD) {
        // The export falls back to Unavailable; the next caller retries the load.
    }
    const auto target = index < kProxyExportCount ? ProxyTargets[index] : nullptr;
    if (target) return target;
    return reinterpret_cast<FARPROC>(Unavailable);
}
}
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        ownModule = module;
        HMODULE pinned{};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&DllMain), &pinned);
        // Dispatch, never join a worker under loader lock; static CRT/TLS keeps thread notifications.
        const auto thread = CreateThread(nullptr, 0, Boot, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread); else startupError.store(GetLastError());
    }
    return TRUE;
}

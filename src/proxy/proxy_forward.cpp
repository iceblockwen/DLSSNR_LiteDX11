// Alternative entry point, used when the game's dxgi.dll belongs to another
// mod: forward one system DLL's exports unchanged (names, indices and ordinals
// come from src/proxy/<name>.def through the generated header and stubs) and
// start the body, exactly like the DXGI proxy does.
#include "proxy_common.hpp"

namespace
{
// A missing system module breaks the game anyway; a defined zero keeps the
// proxy from faulting inside an unrelated export.
DWORD WINAPI Unavailable()
{
    return 0;
}
}

extern "C"
{
    alignas(8) FARPROC ProxyTargets[kProxyExportCount]{};

    FARPROC ResolveProxy(unsigned index)
    {
        const auto target = lite::proxy::Resolve(ProxyTargets, index, []
                                                 {
                                                     return lite::proxy::LoadOriginal(kProxyTargetName, kProxyOriginalName);
                                                 });
        return target ? target : reinterpret_cast<FARPROC>(Unavailable);
    }
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        lite::proxy::Attach(module);
    }
    return TRUE;
}
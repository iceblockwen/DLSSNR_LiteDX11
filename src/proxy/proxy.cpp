// Thin DXGI proxy: forwards every system export unchanged and starts the body.
// No graphics/UI/model ownership in this DLL; the body wraps factories through
// its own detour of the system CreateDXGIFactory* exports.
#include "proxy_common.hpp"
#include <unknwn.h>

namespace
{
// Explicit values for a missing system DXGI, as in ReShade. A load failure
// never publishes these, so the next call retries the load.
HRESULT WINAPI Unavailable()
{
    return E_NOTIMPL;
}

HRESULT WINAPI UnavailableObject(REFIID, void** output)
{
    if (output)
    {
        *output = nullptr;
    }
    return E_NOTIMPL;
}

HRESULT WINAPI UnavailableFlagsObject(UINT, REFIID, void** output)
{
    if (output)
    {
        *output = nullptr;
    }
    return E_NOTIMPL;
}

BOOL WINAPI UnavailableString(LPCSTR, ULONG*, LPSTR, bool)
{
    return FALSE;
}

BOOL WINAPI UnavailableValue(LPCSTR, UINT64*)
{
    return FALSE;
}

HRESULT WINAPI UnavailableDevice(HMODULE, void*, void*, UINT, const void*, UINT, void**)
{
    return E_NOTIMPL;
}

HRESULT WINAPI UnavailableLayered(void*, UINT, void*, REFIID, void**)
{
    return E_NOTIMPL;
}

HRESULT WINAPI UnavailableLayers(const void*, UINT)
{
    return E_NOTIMPL;
}

HRESULT WINAPI UnavailablePointer(void*)
{
    return E_NOTIMPL;
}

void WINAPI EtwRundown()
{
}

// Indexed like src/proxy/dxgi.def.
const FARPROC kFallbacks[]{
    reinterpret_cast<FARPROC>(UnavailableObject),      // CreateDXGIFactory
    reinterpret_cast<FARPROC>(UnavailableObject),      // CreateDXGIFactory1
    reinterpret_cast<FARPROC>(UnavailableFlagsObject), // CreateDXGIFactory2
    reinterpret_cast<FARPROC>(Unavailable),            // DXGIDeclareAdapterRemovalSupport
    reinterpret_cast<FARPROC>(UnavailableFlagsObject), // DXGIGetDebugInterface1
    reinterpret_cast<FARPROC>(UnavailableString),      // CompatString
    reinterpret_cast<FARPROC>(UnavailableValue),       // CompatValue
    reinterpret_cast<FARPROC>(UnavailableDevice),      // DXGID3D10CreateDevice
    reinterpret_cast<FARPROC>(UnavailableLayered),     // DXGID3D10CreateLayeredDevice
    reinterpret_cast<FARPROC>(UnavailableLayers),      // DXGID3D10GetLayeredDeviceSize
    reinterpret_cast<FARPROC>(UnavailableLayers),      // DXGID3D10RegisterLayers
    reinterpret_cast<FARPROC>(EtwRundown),             // DXGID3D10ETWRundown
    reinterpret_cast<FARPROC>(UnavailablePointer),     // DXGIDumpJournal
    reinterpret_cast<FARPROC>(UnavailablePointer),     // DXGIReportAdapterConfiguration
    reinterpret_cast<FARPROC>(Unavailable),            // DXGIDisableVBlankVirtualization
};
static_assert(sizeof(kFallbacks) / sizeof(kFallbacks[0]) == kProxyExportCount,
              "fallback table must cover every export declared in dxgi.def");
}

extern "C"
{
    alignas(8) FARPROC DxgiTargets[kProxyExportCount]{};

    FARPROC ResolveDxgi(unsigned index)
    {
        const auto target = lite::proxy::Resolve(DxgiTargets, index, []
                                                 {
                                                     return lite::proxy::LoadSystem(L"dxgi.dll");
                                                 });
        return target ? target : kFallbacks[index];
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
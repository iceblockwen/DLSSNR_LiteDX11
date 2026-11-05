// SPDX-License-Identifier: GPL-3.0-or-later
// Lite-style thin DXGI proxy. No graphics/UI/model ownership in this DLL.
#include "graphics.hpp"
#include "bootstrap.hpp"
#include "proxy_common.hpp"
#include "factory_origin.hpp"
#include <intrin.h>
#include <mutex>
#include <atomic>
#include <cstdio>
namespace {
HMODULE ownModule{}, systemModule{};
std::once_flag systemOnce;
std::atomic<HMODULE> runtimeModule{};
std::atomic<DWORD> startupError{};
std::atomic<DWORD> systemLoadError{};
const lite::proxy::Entry kEntry{&runtimeModule,&startupError,L"DLSSNR_LiteDX11",
 L"DLSSNR_LiteDX11.ini",L"DLSSNR_LiteDX11.log",L"nvngx.dll_litedx11.dll",
 L"system DXGI forwarding remains available."};
std::atomic<FARPROC> originalFactories[3]{};
constexpr const char* exports[] = {
 "CreateDXGIFactory", "CreateDXGIFactory1", "CreateDXGIFactory2", "DXGIDeclareAdapterRemovalSupport", "DXGIGetDebugInterface1",
 "CompatString", "CompatValue", "DXGID3D10CreateDevice", "DXGID3D10CreateLayeredDevice", "DXGID3D10GetLayeredDeviceSize",
 "DXGID3D10RegisterLayers", "DXGID3D10ETWRundown", "DXGIDumpJournal", "DXGIReportAdapterConfiguration", "DXGIDisableVBlankVirtualization"
};
void WrapFactory(REFIID iid, void** output) {
 const auto module = runtimeModule.load(); if (!module) return;
 using Attach = BOOL(WINAPI*)(REFIID, void**);
 if (const auto attach = reinterpret_cast<Attach>(GetProcAddress(module, "LiteAttachFactory"))) attach(iid, output);
}
HRESULT FactoryUnavailable(void** out) { if(!out) return E_POINTER; *out=nullptr; const auto error=systemLoadError.load(); return error?HRESULT_FROM_WIN32(error):E_NOTIMPL; }
HRESULT WINAPI CreateFactory(REFIID iid, void** out) { const bool internal=lite::NativeFactoryCaller(_ReturnAddress());const auto native=originalFactories[0].load(); if(!native)return FactoryUnavailable(out); auto hr = reinterpret_cast<HRESULT(WINAPI*)(REFIID,void**)>(native)(iid,out); if (SUCCEEDED(hr)&&!internal) WrapFactory(iid,out); return hr; }
HRESULT WINAPI CreateFactory1(REFIID iid, void** out) { const bool internal=lite::NativeFactoryCaller(_ReturnAddress());const auto native=originalFactories[1].load(); if(!native)return FactoryUnavailable(out); auto hr = reinterpret_cast<HRESULT(WINAPI*)(REFIID,void**)>(native)(iid,out); if (SUCCEEDED(hr)&&!internal) WrapFactory(iid,out); return hr; }
HRESULT WINAPI CreateFactory2(UINT flags, REFIID iid, void** out) { const bool internal=lite::NativeFactoryCaller(_ReturnAddress());const auto native=originalFactories[2].load(); if(!native)return FactoryUnavailable(out); auto hr = reinterpret_cast<HRESULT(WINAPI*)(UINT,REFIID,void**)>(native)(flags,iid,out); if (SUCCEEDED(hr)&&!internal) WrapFactory(iid,out); return hr; }
HRESULT WINAPI Unavailable() { return E_NOTIMPL; }
HRESULT WINAPI UnavailableDebug(UINT, REFIID, void** output) { if(output)*output=nullptr; return E_NOTIMPL; }
BOOL WINAPI UnavailableString(LPCSTR, ULONG*, LPSTR, bool) { return FALSE; }
BOOL WINAPI UnavailableValue(LPCSTR, UINT64*) { return FALSE; }
HRESULT WINAPI UnavailableDevice(HMODULE, IDXGIFactory*, IDXGIAdapter*, UINT, const void*, UINT, void**) { return E_NOTIMPL; }
HRESULT WINAPI UnavailableLayered(IDXGIAdapter*, UINT, void*, REFIID, void**) { return E_NOTIMPL; }
HRESULT WINAPI UnavailableSize(const void*, UINT) { return E_NOTIMPL; }
HRESULT WINAPI UnavailableLayers(const void*, UINT) { return E_NOTIMPL; }
HRESULT WINAPI UnavailablePointer(void*) { return E_NOTIMPL; }
void WINAPI EtwRundown() {}
DWORD WINAPI Boot(void*) { return lite::proxy::BootRuntime(ownModule,kEntry); }
}
extern "C" {
alignas(8) FARPROC DxgiTargets[15]{};
FARPROC ResolveDxgi(unsigned index) {
 // call_once leaves the flag unset when the body throws, so a failed load is
 // retried by the next caller. The DWORD carries the reason out to the export.
 DWORD error=ERROR_SUCCESS;
 try {
  std::call_once(systemOnce, [] {
  wchar_t path[MAX_PATH]{};
  if (!GetSystemDirectoryW(path,MAX_PATH)) throw DWORD(GetLastError());
  if (wcscat_s(path,L"\\dxgi.dll")) throw DWORD(ERROR_INSUFFICIENT_BUFFER);
  const auto module=LoadLibraryExW(path,nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!module) throw DWORD(GetLastError());
  if (module==ownModule) throw DWORD(ERROR_INVALID_HANDLE);
  systemModule=module;
  for (unsigned i=0;i<15;++i) {
   auto fn=GetProcAddress(module,exports[i]);
   if (i<3) { originalFactories[i]=fn; const FARPROC wrappers[]={reinterpret_cast<FARPROC>(CreateFactory),reinterpret_cast<FARPROC>(CreateFactory1),reinterpret_cast<FARPROC>(CreateFactory2)}; if(fn) fn=wrappers[i]; }
   InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(&DxgiTargets[i]),reinterpret_cast<PVOID>(fn));
  }
  });
 } catch (DWORD code) { error=code; } catch (...) { error=ERROR_GEN_FAILURE; }
 systemLoadError=error;
 const auto target=index<15?DxgiTargets[index]:nullptr;
 if(target) return target;
 // Known signatures use the same explicit fallback values as ReShade. Failed
 // loading never publishes these pointers, so the next call retries initialization.
 const FARPROC fallback[]={reinterpret_cast<FARPROC>(CreateFactory),reinterpret_cast<FARPROC>(CreateFactory1),reinterpret_cast<FARPROC>(CreateFactory2),
  reinterpret_cast<FARPROC>(Unavailable),reinterpret_cast<FARPROC>(UnavailableDebug),
  reinterpret_cast<FARPROC>(UnavailableString),reinterpret_cast<FARPROC>(UnavailableValue),
  reinterpret_cast<FARPROC>(UnavailableDevice),reinterpret_cast<FARPROC>(UnavailableLayered),
  reinterpret_cast<FARPROC>(UnavailableSize),reinterpret_cast<FARPROC>(UnavailableLayers),reinterpret_cast<FARPROC>(EtwRundown),
  reinterpret_cast<FARPROC>(UnavailablePointer),reinterpret_cast<FARPROC>(UnavailablePointer),reinterpret_cast<FARPROC>(Unavailable)};
 if(index<15) return fallback[index];
 RaiseFailFastException(nullptr,nullptr,0);
 return nullptr;
}
}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID) {
 if(reason==DLL_PROCESS_ATTACH) {
  ownModule=module;
  HMODULE pinned{}; GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(&DllMain),&pinned);
  // Like Lite: dispatch, never join a worker under loader lock. Static CRT/TLS keeps thread notifications.
  const auto thread=CreateThread(nullptr,0,Boot,nullptr,0,nullptr);
  if(thread) CloseHandle(thread); else startupError=GetLastError();
 }
 return TRUE;
}

// SPDX-License-Identifier: GPL-3.0-or-later
#include "model_runtime.hpp"
#include "ngx_initialization.hpp"
#include "../graphics.hpp"
#include <dxgi1_6.h>
#include <filesystem>
#include <vector>
extern "C" int __cdecl LiteNrCreate(lite::nr::CreateFn,ID3D12GraphicsCommandList*,int,const void*,void**);
extern "C" int __cdecl LiteNrEvaluate(lite::nr::EvaluateFn,ID3D12GraphicsCommandList*,const void*,const void*,void*);
extern "C" int __cdecl LiteNrRelease(lite::nr::ReleaseFn,void*);
namespace lite::nr {
namespace {
std::wstring ModulePath(HMODULE module) {
    wchar_t path[1024]{};return GetModuleFileNameW(module,path,1024)?path:L"";
}
std::wstring Version(const wchar_t* path){
 DWORD ignored{};const auto size=GetFileVersionInfoSizeW(path,&ignored);if(!size)return L"Unavailable";
 std::vector<BYTE> data(size);if(!GetFileVersionInfoW(path,0,size,data.data()))return L"Unavailable";
 VS_FIXEDFILEINFO* info{};UINT bytes{};if(!VerQueryValueW(data.data(),L"\\",reinterpret_cast<void**>(&info),&bytes)||bytes<sizeof(*info)||info->dwSignature!=0xfeef04bd)return L"Unavailable";
 wchar_t text[64]{};swprintf_s(text,L"%u.%u.%u.%u",HIWORD(info->dwFileVersionMS),LOWORD(info->dwFileVersionMS),HIWORD(info->dwFileVersionLS),LOWORD(info->dwFileVersionLS));return text;
}
std::wstring DriverDirectory() {
    HKEY key{};if(RegOpenKeyExW(HKEY_LOCAL_MACHINE,L"System\\CurrentControlSet\\Services\\nvlddmkm\\NGXCore",0,KEY_READ,&key)!=ERROR_SUCCESS)return {};
    wchar_t text[1024]{};DWORD bytes=sizeof(text),type{};
    const auto rc=RegQueryValueExW(key,L"NGXPath",nullptr,&type,reinterpret_cast<BYTE*>(text),&bytes);RegCloseKey(key);
    if(rc!=ERROR_SUCCESS || type!=REG_SZ || !bytes || bytes>sizeof(text))return {};
    text[1023]=0;return text;
}
std::wstring Find(const std::wstring& root,const wchar_t* name) {
    if(auto module=GetModuleHandleW(name))return ModulePath(module);
    wchar_t exe[1024]{},system[1024]{};GetModuleFileNameW(nullptr,exe,1024);GetSystemDirectoryW(system,1024);
    const std::filesystem::path folders[]{std::filesystem::path(exe).parent_path(),root,system,DriverDirectory()};
    for(const auto& folder:folders) {
        if(folder.empty())continue;const auto file=folder/name;std::error_code ec;
        if(std::filesystem::is_regular_file(file,ec))return file.wstring();
    }
    return {};
}
HMODULE LoadPinned(const wchar_t* path) {
    const auto loaded=LoadLibraryExW(path,nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if(!loaded)return nullptr;
    HMODULE pinned{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(loaded),&pinned)) {FreeLibrary(loaded);return nullptr;}
    return loaded;
}
}
ModelRuntime::ModelRuntime(std::wstring root):directory(std::move(root)) {}
HRESULT ModelRuntime::Fail(HRESULT hr,const wchar_t* reason,unsigned result) {
    const bool changed=status.state!=State::Failed||status.error!=hr||status.ngxResult!=result;
    unavailable=true;status.error=hr;status.ngxResult=result;status.state=State::Failed;if(reason!=status.reason)wcsncpy_s(status.reason,reason,_TRUNCATE);
    if(changed){wchar_t text[384]{};swprintf_s(text,L"NR runtime: %ls (vendor 0x%04X, HRESULT 0x%08lX, NGX 0x%08X)",status.reason,status.vendor,static_cast<unsigned long>(hr),result);Log(text);}return hr;
}
HRESULT ModelRuntime::Discover() {
    std::lock_guard lock(mutex);
    if(unavailable)return status.error; // Discovery cannot clear a latched initialization failure.
    auto path=Find(directory,L"_nvngx.dll");if(path.empty())path=Find(directory,L"nvngx.dll");
    wcsncpy_s(status.corePath,path.c_str(),_TRUNCATE);
    // The model is an explicit plugin dependency. Do not pick a snippet from an unrelated directory.
    const auto file=std::filesystem::path(directory)/L"nvngx_dlssnr.dll";std::error_code ec;
    const auto modelFile=std::filesystem::is_regular_file(file,ec)?file.wstring():L"";
    wcsncpy_s(status.modelPath,modelFile.c_str(),_TRUNCATE);
    wcsncpy_s(status.coreVersion,path.empty()?L"Not found":Version(status.corePath).c_str(),_TRUNCATE);
    wcsncpy_s(status.modelVersion,modelFile.empty()?L"Not found":Version(status.modelPath).c_str(),_TRUNCATE);
    if(path.empty() || modelFile.empty()) {
        status.state=State::Waiting;status.error=HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
        wcscpy_s(status.reason,path.empty()?L"NVIDIA NGX core not found; original game frame preserved":L"nvngx_dlssnr.dll missing beside DLSSNR_LiteDX11");return S_FALSE;
    }
    if(!device) {status.state=State::Waiting;status.error=S_OK;wcscpy_s(status.reason,L"Runtime files discovered; device/model execution not initialized");}
    return S_OK;
}
HRESULT ModelRuntime::Adapter(ID3D12Device* d,unsigned& vendor,std::wstring& name) {
    if(!d)return E_POINTER;ComPtr<IDXGIFactory4> factory;auto hr=CreateDXGIFactory1(IID_PPV_ARGS(&factory));if(FAILED(hr))return hr;
    ComPtr<IDXGIAdapter1> adapter;hr=factory->EnumAdapterByLuid(d->GetAdapterLuid(),IID_PPV_ARGS(&adapter));if(FAILED(hr))return hr;
    DXGI_ADAPTER_DESC1 desc{};hr=adapter->GetDesc1(&desc);if(FAILED(hr))return hr;vendor=desc.VendorId;name=desc.Description;return S_OK;
}
HRESULT ModelRuntime::Initialize(ID3D12Device* d) {
    std::lock_guard lock(mutex);if(!d)return E_POINTER;
    if(device) return device.Get()==d?S_OK:E_NOTIMPL;
    if(unavailable)return status.error;
    std::wstring name;auto hr=Adapter(d,status.vendor,name);if(FAILED(hr))return Fail(hr,L"Cannot identify NR adapter");
    wcsncpy_s(status.adapter,name.c_str(),_TRUNCATE);
    if(status.vendor!=0x10de)return Fail(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),L"This adapter is not NVIDIA; real NGX/NR validation is unavailable");
    hr=Discover();if(hr!=S_OK)return Fail(status.error,status.reason);
    core=LoadPinned(status.corePath);if(!core)return Fail(HRESULT_FROM_WIN32(GetLastError()),L"NGX core failed to load");
    auto caps=reinterpret_cast<CoreCapsFn>(GetProcAddress(core,"NVSDK_NGX_D3D12_GetCapabilityParameters"));
    if(!caps)return Fail(HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND),L"Core lacks DX12 capability export");
    destroyParameters=reinterpret_cast<decltype(destroyParameters)>(GetProcAddress(core,"NVSDK_NGX_D3D12_DestroyParameters"));
    if(!destroyParameters)return Fail(HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND),L"Core lacks DX12 parameter destruction export");
    // An existing game's capability block prevents intrusive core reinitialization.
    auto coreInit=reinterpret_cast<CoreInitFn>(GetProcAddress(core,"NVSDK_NGX_D3D12_Init"));
    unsigned rc=AcquireCapability(caps,coreInit,directory.c_str(),d,parameters);
    if(rc!=1 || !parameters)return Fail(E_FAIL,L"NGX core refused capability parameters",rc);
    writer.Bind(parameters);writer.Discover();
    if(!writer.Verified()){wchar_t text[192]{};swprintf_s(text,L"NR float setter: slot %d (unverified Lite header fallback)",writer.Slot());Log(text);}
    model=LoadPinned(status.modelPath);if(!model)return Fail(HRESULT_FROM_WIN32(GetLastError()),L"NR model failed to load");
    NgxLookupBypass direct;
    auto init=reinterpret_cast<ModelInitFn>(GetProcAddress(model,"NVSDK_NGX_D3D12_Init_Ext"));
    create=reinterpret_cast<CreateFn>(GetProcAddress(model,"NVSDK_NGX_D3D12_CreateFeature"));
    evaluate=reinterpret_cast<EvaluateFn>(GetProcAddress(model,"NVSDK_NGX_D3D12_EvaluateFeature"));
    release=reinterpret_cast<ReleaseFn>(GetProcAddress(model,"NVSDK_NGX_D3D12_ReleaseFeature"));
    if(!init || !create || !evaluate || !release)return Fail(HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND),L"NR model lacks required DX12 exports");
    rc=InitializeModel(init,directory.c_str(),d,parameters);
    if(rc!=1)return Fail(E_FAIL,L"NR Init_Ext refused initialization",rc);
    device=d;status.state=State::Ready;status.error=S_OK;status.ngxResult=rc;wcscpy_s(status.reason,L"Runtime initialized; feature/output still requires validation");return S_OK;
}
ModelStatus ModelRuntime::Snapshot() const {std::lock_guard lock(mutex);return status;}
void ModelRuntime::RetryFailedInitialization() {
    std::lock_guard lock(mutex);
    if(device||!unavailable)return;
    // No features exist before binding a device. Only retire our own allocated
    // capability map; the game's NGX core and parameter maps remain untouched.
    if(parameters){
        const auto rc=destroyParameters?destroyParameters(parameters):0xBAD0000Bu;
        if(rc!=1){Fail(E_FAIL,L"Cannot retire failed NGX capability map",rc);return;}
    }
    parameters=nullptr;writer.Bind(nullptr);create=nullptr;evaluate=nullptr;release=nullptr;
    status.state=State::Waiting;status.error=S_OK;status.ngxResult=0;
    wcscpy_s(status.reason,L"NR re-enabled; runtime initialization may be retried");
    unavailable=false;
}
bool ModelRuntime::OwnsDevice(ID3D12Device* d) const {std::lock_guard lock(mutex);return d&&device.Get()==d;}
unsigned ModelRuntime::Create(ID3D12GraphicsCommandList* list,void** out) {std::lock_guard lock(mutex);return create?unsigned(LiteNrCreate(create,list,18,parameters,out)):0xBAD0000Bu;}
unsigned ModelRuntime::Evaluate(ID3D12GraphicsCommandList* list,void* feature) {std::lock_guard lock(mutex);return evaluate?unsigned(LiteNrEvaluate(evaluate,list,feature,parameters,nullptr)):0xBAD0000Bu;}
unsigned ModelRuntime::Release(void* feature) {
    std::lock_guard lock(mutex);
    const auto rc=release?unsigned(LiteNrRelease(release,feature)):0xBAD0000Bu;
    if(rc!=1){wchar_t text[128]{};swprintf_s(text,L"NR feature release failed: NGX=0x%08X",rc);Log(text);}
    return rc;
}
void ModelRuntime::SetFloat(const char* key,float v){std::lock_guard lock(mutex);writer.Float(key,v);}
void ModelRuntime::SetUnsigned(const char* key,unsigned v){std::lock_guard lock(mutex);writer.Unsigned(key,v);}
void ModelRuntime::SetResource(const char* key,ID3D12Resource* v){std::lock_guard lock(mutex);writer.Resource(key,v);}
ModelRuntime& Runtime() {
    static auto* r=new ModelRuntime(std::filesystem::path(lite::Snapshot().pluginPath).parent_path().wstring());return *r;
}
void DiscoverModelRuntime() {try {Runtime().Discover();const auto s=Runtime().Snapshot();wchar_t text[1400]{};swprintf_s(text,L"NGX core: %ls (version %ls)",s.corePath[0]?s.corePath:L"Not found",s.coreVersion);Log(text);swprintf_s(text,L"NR model: %ls (version %ls)",s.modelPath[0]?s.modelPath:L"Not found",s.modelVersion);Log(text);Log(s.reason);}catch(...) {Log(L"NR runtime discovery failed; original game frame preserved");}}
}

// Lite loader.cpp/hooks.cpp startup, checked against Opt's dllmain.cpp and
// LibraryLoad_Hooks.cpp. All initialization runs after DLL_PROCESS_ATTACH.
#include "bootstrap.hpp"
#include "graphics.hpp"
#include "config_writer.hpp"
#include "nr/model_runtime.hpp"
#include <mutex>

namespace
{
HMODULE own{};
std::once_flag once;
DWORD startupError{};
}

extern "C" __declspec(dllexport) DWORD WINAPI Start(void*)
{
    std::call_once(once, []
                   {
                       try
                       {
                           wchar_t path[MAX_PATH]{};
                           if (!GetSystemDirectoryW(path, MAX_PATH) || wcscat_s(path, L"\\dxgi.dll"))
                           {
                               throw DWORD(ERROR_BAD_PATHNAME);
                           }
                           const auto system = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
                           if (!system)
                           {
                               throw GetLastError();
                           }

                           lite::Initialize(own, system);
                           const bool enabled = lite::Enabled();
                           // Install capture before any future heavyweight model preload.
                           if (enabled)
                           {
                               lite::InstallCapture();
                           }
                           lite::StartConfigWriter();
                           if (enabled)
                           {
                               lite::nr::DiscoverModelRuntime();
                           }
                       }
                       catch (DWORD error)
                       {
                           lite::Fail(error);
                           startupError = error;
                       }
                       catch (HRESULT error)
                       {
                           lite::Fail(DWORD(error));
                           startupError = DWORD(error);
                       }
                       catch (...)
                       {
                           lite::Fail(ERROR_DLL_INIT_FAILED);
                           startupError = ERROR_DLL_INIT_FAILED;
                       }
                   });
    return startupError;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        own = module;
    }
    return TRUE;
}
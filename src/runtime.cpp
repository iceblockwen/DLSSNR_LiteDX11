#include "runtime.hpp"
#include "graphics.hpp"
#include "nr/capture11.hpp"
#include "nr/depth11.hpp"
#include "nr/model_settings.hpp"
#include "nr/model_runtime.hpp"
#include "config.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <cstdio>
#include <share.h>

#define LITE_WIDEN2(text) L##text
#define LITE_WIDEN(text) LITE_WIDEN2(text)

namespace
{
std::mutex statusMutex;
lite::Status status;
std::atomic<bool> enabled{true}, usable{true}, logging{true}, skipSmallSwapchains{};
std::atomic<unsigned> detail{1};
// Present, window messages and capture paths only enqueue. One detached worker
// owns the file; the pinned module keeps this state for the process lifetime.
struct LogQueue
{
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::wstring> lines;
    std::wstring path;
    bool started{};
    void Run()
    {
        FILE* file{};
        std::unique_lock lock(mutex);
        for (;;)
        {
            ready.wait(lock, [&]
                       {
                           return !lines.empty();
                       });
            auto batch = std::move(lines);
            lines.clear();
            // One file per process run: the previous session's events never mix in.
            if (!file)
            {
                file = _wfsopen(path.c_str(), L"w, ccs=UTF-8", _SH_DENYNO);
            }
            lock.unlock();
            if (file)
            {
                for (const auto& line : batch)
                {
                    fwprintf(file, L"%ls\n", line.c_str());
                }
                fflush(file);
            }
            lock.lock();
        }
    }
};
LogQueue& Logs()
{
    static auto* queue = new LogQueue;
    return *queue;
}
}
namespace lite
{
void Initialize(HMODULE plugin, HMODULE system)
{
    std::wstring ini;
    unsigned level = 1;
    {
        std::lock_guard lock(statusMutex);
        GetModuleFileNameW(plugin, status.pluginPath, 1024);
        GetModuleFileNameW(system, status.systemPath, 1024);
        const auto folder = std::filesystem::path(status.pluginPath).parent_path();
        {
            std::lock_guard logLock(Logs().mutex);
            Logs().path = (folder / L"DLSSNR_LiteDX11.log").wstring();
        }
        ini = InitializeConfigStorage((folder / L"DLSSNR_LiteDX11.ini").wstring());
        status.enabled = GetPrivateProfileIntW(L"General", L"Enabled", 1, ini.c_str()) != 0;
        const int configured = GetPrivateProfileIntW(L"Diagnostics", L"Level", 1, ini.c_str());
        level = configured < 0 ? 0u : (configured > 2 ? 2u : unsigned(configured));
        status.logging = level >= 1;
        status.loader = status.enabled ? State::Ready : State::Disabled;
        enabled = status.enabled;
        usable = status.enabled;
        logging = status.logging;
        detail.store(level);
        skipSmallSwapchains = GetPrivateProfileIntW(L"Diagnostics", L"SkipSmallSwapchains", 0, ini.c_str()) != 0;
    }
    nr::ConfigureModel(ini.c_str());
    // Read at startup: it decides whether any depth/motion hook is installed at all.
    nr::SetNativeCaptureEnabled(GetPrivateProfileIntW(L"NeuralRendering", L"NativeCapture", 0, ini.c_str()) != 0);
    auto flowFloat = [&](const wchar_t* key, const wchar_t* initial)
    {
        wchar_t value[64]{};
        GetPrivateProfileStringW(L"Resources", key, initial, value, 64, ini.c_str());
        return float(_wtof(value));
    };
    nr::DenseFlowSettings flowOptions;
    flowOptions.fov = flowFloat(L"FlowFov", L"60");
    flowOptions.farPlane = flowFloat(L"FlowFarPlane", L"1000");
    flowOptions.depthMultiplier = flowFloat(L"FlowDepthMultiplier", L"1");
    flowOptions.logarithmic = GetPrivateProfileIntW(L"Resources", L"FlowLogDepth", 0, ini.c_str()) != 0;
    flowOptions.depthScaleX = flowFloat(L"FlowDepthScaleX", L"1");
    flowOptions.depthScaleY = flowFloat(L"FlowDepthScaleY", L"1");
    flowOptions.depthOffsetX = flowFloat(L"FlowDepthOffsetX", L"0");
    flowOptions.depthOffsetY = flowFloat(L"FlowDepthOffsetY", L"0");
    flowOptions.depthPixelOffsetX = flowFloat(L"FlowDepthPixelOffsetX", L"0");
    flowOptions.depthPixelOffsetY = flowFloat(L"FlowDepthPixelOffsetY", L"0");
    nr::SetFlowOptions(flowOptions, false);
    nr::SetDepthContrast(GetPrivateProfileIntW(L"Diagnostics", L"DepthContrast", 0, ini.c_str()) != 0, false);
    nr::SetDepthFlip(GetPrivateProfileIntW(L"Resources", L"DepthUpsideDown", 0, ini.c_str()) != 0, false);
    wchar_t previewScale[32]{};
    GetPrivateProfileStringW(L"Diagnostics", L"ResourceScale", L"1", previewScale, 32, ini.c_str());
    nr::SetDisplayScale(static_cast<float>(_wtof(previewScale)), false);
}
void Fail(DWORD error)
{
    std::lock_guard lock(statusMutex);
    status.loader = State::Failed;
    status.error = error;
    usable = false;
}
Status Snapshot()
{
    std::lock_guard lock(statusMutex);
    return status;
}
bool Enabled()
{
    return enabled.load(std::memory_order_relaxed);
}
bool Usable()
{
    return usable.load(std::memory_order_relaxed);
}
bool SkipSmallSwapchains()
{
    return skipSmallSwapchains.load(std::memory_order_relaxed);
}
Detail DetailLevel() noexcept
{
    return static_cast<Detail>(detail.load(std::memory_order_relaxed));
}
void LogVerbose(const wchar_t* message) noexcept
{
    if (detail.load(std::memory_order_relaxed) < 2)
    {
        return;
    }
    Log(message);
}
void Log(const wchar_t* message) noexcept
{
    try
    {
        if (!message)
        {
            return;
        }
        if (!logging)
        {
            return;
        }
        auto& queue = Logs();
        {
            std::lock_guard lock(queue.mutex);
            if (queue.path.empty() || queue.lines.size() >= 4096)
            {
                return;
            }
            queue.lines.emplace_back(message);
            if (!queue.started)
            {
                std::thread([&queue]
                            {
                                queue.Run();
                            })
                    .detach();
                queue.started = true;
            }
        }
        queue.ready.notify_one();
    }
    catch (...)
    {
    }
}
}

namespace
{
HMODULE ownModule{};
std::once_flag startupOnce;
DWORD startupError{};
}

// Loaded by the proxy's Boot worker: never runs under loader lock. The body
// reports failures through lite::Fail, and the return value is the proxy's
// startup result.
extern "C" __declspec(dllexport) DWORD WINAPI Start(void*)
{
    std::call_once(startupOnce, []
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

                           lite::Initialize(ownModule, system);
                           const bool active = lite::Enabled();
                           // Install capture before any future heavyweight model preload.
                           if (active)
                           {
                               lite::InstallCapture();
                           }
                           lite::StartConfigWriter();
                           if (active)
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
        ownModule = module;
    }
    return TRUE;
}
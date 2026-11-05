#pragma once
#include <windows.h>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace lite
{
// Production instance lives in the pinned DLL for the process lifetime.
// No join or file I/O from DllMain, Present, or a window message.
class ConfigWriter
{
  public:
    using Write = std::function<bool(const std::wstring&, const std::wstring&, const std::wstring&)>;
    explicit ConfigWriter(Write write, std::function<void()> error = {});
    ~ConfigWriter();
    void Queue(const wchar_t* section, const wchar_t* key, const wchar_t* value);
    void Flush(); // Wake the writer, without waiting for disk.
  private:
    using Key = std::pair<std::wstring, std::wstring>;
    struct Value
    {
        std::wstring text;
        unsigned attempts{};
    };
    std::mutex mutex;
    std::condition_variable changed;
    std::map<Key, Value> pending;
    std::map<Key, std::wstring> saved;
    std::chrono::steady_clock::time_point due{}, first{};
    Write write;
    std::function<void()> error;
    bool stopping{}, writing{};
    std::thread worker;
    void Run();
};
void StartConfigWriter();
void QueueConfig(const wchar_t* section, const wchar_t* key, const wchar_t* value) noexcept;
void FlushConfig() noexcept;
}

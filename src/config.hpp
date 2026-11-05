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
// Where the INI lives: the game folder unless an override or a writable profile
// copy wins. Selection is read-only; directory creation and seeding run on the
// save worker.
struct ConfigSelection
{
    std::wstring sourceFile, saveFile, fallbackFile;
    bool explicitDirectory{}, userProfile{};
};
struct ConfigSaveStatus : ConfigSelection
{
    DWORD error{}, fallbackCause{};
};
ConfigSelection SelectConfig(const std::wstring& pluginFile, const std::wstring& profileFile,
                             const std::wstring& overrideDirectory);
class ConfigStorage
{
  public:
    explicit ConfigStorage(ConfigSelection selection, DWORD selectionError = ERROR_SUCCESS);
    // Called by the single background save worker. Snapshot never waits on I/O.
    bool Write(const std::wstring& section, const std::wstring& key, const std::wstring& value);
    ConfigSaveStatus Snapshot() const;

  private:
    mutable std::mutex mutex;
    ConfigSaveStatus status;
};

// Debounced writer. The production instance lives in the pinned DLL for the
// process lifetime: no join or file I/O from DllMain, Present or a window message.
class ConfigWriter
{
  public:
    using Write = std::function<bool(const std::wstring&, const std::wstring&, const std::wstring&)>;
    explicit ConfigWriter(Write write);
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
    bool stopping{}, writing{};
    std::thread worker;
    void Run();
};

std::wstring InitializeConfigStorage(const std::wstring& pluginFile);
bool WriteConfigValue(const std::wstring&, const std::wstring&, const std::wstring&);
ConfigSaveStatus ConfigStorageSnapshot();
void StartConfigWriter();
void QueueConfig(const wchar_t* section, const wchar_t* key, const wchar_t* value) noexcept;
void FlushConfig() noexcept;
}
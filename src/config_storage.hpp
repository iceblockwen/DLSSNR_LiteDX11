#pragma once
#include <windows.h>
#include <mutex>
#include <string>

namespace lite
{
struct ConfigSelection
{
    std::wstring sourceFile, saveFile, fallbackFile;
    bool explicitDirectory{}, userProfile{};
};
struct ConfigSaveStatus : ConfigSelection
{
    DWORD error{}, fallbackCause{};
};
// Selection is read-only. Directory creation and seeding run on the save worker.
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
std::wstring InitializeConfigStorage(const std::wstring& pluginFile);
bool WriteConfigValue(const std::wstring&, const std::wstring&, const std::wstring&);
ConfigSaveStatus ConfigStorageSnapshot();
}

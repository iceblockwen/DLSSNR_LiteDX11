// Configuration storage (where the INI lives) and the debounced writer that
// persists it. Both halves share one translation unit: the writer is the only
// caller of WriteConfigValue, and the panel only ever reads a snapshot.
#include "config.hpp"
#include "runtime.hpp"
#include <algorithm>
#include <atomic>
#include <cwctype>
#include <filesystem>
#include <utility>

namespace lite
{
// ---- selection, seeding and fallback ----
namespace
{
std::atomic<ConfigStorage*> storage{};
std::wstring Environment(const wchar_t* name)
{
    const auto size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size)
    {
        return {};
    }
    std::wstring result(size, L'\0');
    const auto written = GetEnvironmentVariableW(name, result.data(), size);
    if (!written || written >= size)
    {
        return {};
    }
    result.resize(written);
    return result;
}
std::wstring Expand(const std::wstring& text)
{
    const auto size = ExpandEnvironmentStringsW(text.c_str(), nullptr, 0);
    if (!size || size > 32767)
    {
        throw DWORD(ERROR_INVALID_NAME);
    }
    std::wstring result(size, L'\0');
    const auto written = ExpandEnvironmentStringsW(text.c_str(), result.data(), size);
    if (!written || written > size)
    {
        throw DWORD(ERROR_INVALID_NAME);
    }
    result.resize(written - 1);
    return result;
}
bool File(const std::wstring& path)
{
    const auto attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}
bool Equal(const std::wstring& a, const std::wstring& b)
{
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}
// Profile APIs keep ANSI files in their system code page even when called with
// wide strings. Persist internal redirect paths as ASCII UTF-16 hex so an ANSI
// shipped INI cannot lose characters on an English Windows installation.
std::wstring EncodeRedirect(const std::wstring& path)
{
    constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring result;
    result.reserve(path.size() * 4);
    for (const auto c : path)
    {
        for (int shift = 12; shift >= 0; shift -= 4)
        {
            result += digits[(unsigned(c) >> shift) & 15];
        }
    }
    return result;
}
std::wstring ReadRedirect(const std::wstring& profile)
{
    std::wstring text(131069, L'\0');
    text.resize(GetPrivateProfileStringW(L"LiteStorage", L"RedirectedFromUtf16", L"", text.data(), DWORD(text.size()), profile.c_str()));
    if (text.empty() || text.size() % 4)
    {
        return {};
    }
    std::wstring result;
    result.reserve(text.size() / 4);
    unsigned code{};
    for (size_t n = 0; n < text.size(); ++n)
    {
        const auto c = towlower(text[n]);
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f')))
        {
            return {};
        }
        code = (code << 4) | unsigned(c <= L'9' ? c - L'0' : c - L'a' + 10);
        if (n % 4 == 3)
        {
            if (!code)
            {
                return {};
            }
            result += wchar_t(code);
            code = 0;
        }
    }
    return result;
}
DWORD Seed(const std::wstring& target, const std::wstring& source, bool replace)
{
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(target).parent_path(), error);
    if (error)
    {
        return DWORD(error.value());
    }
    if (Equal(target, source) || (!replace && File(target)) || !File(source))
    {
        return ERROR_SUCCESS;
    }
    if (!CopyFileW(source.c_str(), target.c_str(), !replace))
    {
        const auto code = GetLastError();
        return !replace && (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS) ? ERROR_SUCCESS : code;
    }
    // CopyFile preserves a shipped INI's read-only attribute. Only our newly
    // seeded destination is made writable; the source keeps its attributes.
    const auto attrs = GetFileAttributesW(target.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY) &&
        !SetFileAttributesW(target.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY))
    {
        return GetLastError();
    }
    return ERROR_SUCCESS;
}
DWORD WriteAt(const std::wstring& path, const std::wstring& source, bool replace,
              const std::wstring& section, const std::wstring& key, const std::wstring& value)
{
    const auto error = Seed(path, source, replace);
    if (error)
    {
        return error;
    }
    wchar_t current[128]{};
    GetPrivateProfileStringW(section.c_str(), key.c_str(), L"", current, 128, path.c_str());
    if (value == current)
    {
        return ERROR_SUCCESS;
    }
    SetLastError(ERROR_SUCCESS);
    if (WritePrivateProfileStringW(section.c_str(), key.c_str(), value.c_str(), path.c_str()))
    {
        return ERROR_SUCCESS;
    }
    const auto code = GetLastError();
    return code ? code : ERROR_WRITE_FAULT;
}
std::wstring ProfileFile()
{
    const auto appdata = Environment(L"LOCALAPPDATA");
    if (appdata.empty())
    {
        return {};
    }
    std::wstring exe(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, exe.data(), DWORD(exe.size()));
    if (!length || length >= exe.size())
    {
        return {};
    }
    exe.resize(length);
    unsigned long long hash = 14695981039346656037ull;
    for (const auto c : exe)
    {
        hash ^= static_cast<unsigned long long>(towlower(c));
        hash *= 1099511628211ull;
    }
    wchar_t suffix[32]{};
    swprintf_s(suffix, L"-%016llx", hash);
    auto name = std::filesystem::path(exe).stem().wstring();
    return (std::filesystem::path(appdata) / L"DLSSNR_LiteDX11" / L"Profiles" / (name + suffix) / L"DLSSNR_LiteDX11.ini").wstring();
}
}
ConfigSelection SelectConfig(const std::wstring& pluginFile, const std::wstring& profileFile,
                             const std::wstring& overrideDirectory)
{
    ConfigSelection selected;
    selected.sourceFile = pluginFile;
    selected.saveFile = pluginFile;
    selected.fallbackFile = profileFile;
    if (!overrideDirectory.empty())
    {
        auto directory = std::filesystem::path(Expand(overrideDirectory));
        if (directory.is_relative())
        {
            directory = std::filesystem::path(pluginFile).parent_path() / directory;
        }
        selected.saveFile = (directory.lexically_normal() / L"DLSSNR_LiteDX11.ini").wstring();
        selected.explicitDirectory = true;
        if (File(selected.saveFile))
        {
            selected.sourceFile = selected.saveFile;
        }
        if (!profileFile.empty() && File(profileFile) && Equal(selected.saveFile, ReadRedirect(profileFile)))
        {
            selected.sourceFile = profileFile;
            selected.saveFile = profileFile;
            selected.userProfile = true;
        }
    }
    else if (!profileFile.empty() && File(profileFile))
    {
        selected.sourceFile = profileFile;
        selected.saveFile = profileFile;
        selected.userProfile = true;
    }
    return selected;
}
ConfigStorage::ConfigStorage(ConfigSelection selection, DWORD selectionError)
{
    static_cast<ConfigSelection&>(status) = std::move(selection);
    status.fallbackCause = selectionError;
}
ConfigSaveStatus ConfigStorage::Snapshot() const
{
    std::lock_guard lock(mutex);
    return status;
}
bool ConfigStorage::Write(const std::wstring& section, const std::wstring& key, const std::wstring& value)
{
    auto selected = Snapshot();
    // No status mutex during disk I/O: the panel can always read the last result.
    auto error = WriteAt(selected.saveFile, selected.sourceFile, false, section, key, value);
    if (error && !selected.fallbackFile.empty() && !Equal(selected.saveFile, selected.fallbackFile))
    {
        const auto cause = error;
        // Preserve the whole loaded INI, including settings that did not change.
        error = Seed(selected.fallbackFile, File(selected.saveFile) ? selected.saveFile : selected.sourceFile, true);
        // Remember the requested destination so a blocked explicit directory
        // cannot reload its stale copy next launch. A different override wins.
        if (!error)
        {
            error = WriteAt(selected.fallbackFile, selected.fallbackFile, false, L"LiteStorage", L"RedirectedFromUtf16", EncodeRedirect(selected.saveFile));
        }
        if (!error)
        {
            error = WriteAt(selected.fallbackFile, selected.fallbackFile, false, section, key, value);
        }
        if (!error)
        {
            std::lock_guard lock(mutex);
            status.saveFile = selected.fallbackFile;
            status.userProfile = true;
            status.fallbackCause = cause;
        }
    }
    {
        std::lock_guard lock(mutex);
        status.error = error;
    }
    return error == ERROR_SUCCESS;
}
std::wstring InitializeConfigStorage(const std::wstring& pluginFile)
{
    auto directory = Environment(L"DLSSNR_LITEDX11_CONFIG_DIR");
    if (directory.empty())
    {
        std::wstring value(32768, L'\0');
        value.resize(GetPrivateProfileStringW(L"General", L"ConfigDirectory", L"", value.data(), DWORD(value.size()), pluginFile.c_str()));
        directory = std::move(value);
    }
    const auto profile = ProfileFile();
    ConfigSelection selected;
    DWORD selectionError{};
    try
    {
        selected = SelectConfig(pluginFile, profile, directory);
    }
    catch (DWORD code)
    {
        selectionError = code;
        selected = SelectConfig(pluginFile, profile, {});
    }
    catch (const std::filesystem::filesystem_error&)
    {
        selectionError = ERROR_INVALID_NAME;
        selected = SelectConfig(pluginFile, profile, {});
    }
    const auto read = selected.sourceFile;
    storage = new ConfigStorage(std::move(selected), selectionError);
    return read;
}
bool WriteConfigValue(const std::wstring& section, const std::wstring& key, const std::wstring& value)
{
    const auto current = storage.load();
    return current && current->Write(section, key, value);
}
ConfigSaveStatus ConfigStorageSnapshot()
{
    const auto current = storage.load();
    return current ? current->Snapshot() : ConfigSaveStatus{};
}
// ---- debounced writer ----
namespace
{
std::atomic<ConfigWriter*> writer{};
}
ConfigWriter::ConfigWriter(Write output)
    : write(std::move(output)), worker([this]
                                       {
                                           Run();
                                       })
{
}
ConfigWriter::~ConfigWriter()
{
    {
        std::lock_guard lock(mutex);
        stopping = true;
    }
    changed.notify_one();
    worker.join();
}
void ConfigWriter::Queue(const wchar_t* section, const wchar_t* key, const wchar_t* value)
{
    std::lock_guard lock(mutex);
    if (stopping)
    {
        return;
    }
    Key k{section, key};
    auto p = pending.find(k);
    if (p != pending.end() && p->second.text == value)
    {
        return;
    }
    auto old = saved.find(k);
    if (!writing && p == pending.end() && old != saved.end() && old->second == value)
    {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (pending.empty())
    {
        first = now;
    }
    pending[k] = {value, 0};
    due = (std::min)(now + std::chrono::milliseconds(150), first + std::chrono::seconds(1));
    changed.notify_one();
}
void ConfigWriter::Flush()
{
    {
        std::lock_guard lock(mutex);
        due = std::chrono::steady_clock::now();
    }
    changed.notify_one();
}
void ConfigWriter::Run()
{
    std::unique_lock lock(mutex);
    for (;;)
    {
        changed.wait(lock, [&]
                     {
                         return stopping || !pending.empty();
                     });
        if (stopping && pending.empty())
        {
            return;
        }
        if (!stopping && std::chrono::steady_clock::now() < due)
        {
            changed.wait_until(lock, due);
            continue;
        }
        auto batch = std::move(pending);
        pending.clear();
        writing = true;
        lock.unlock();
        for (auto& [key, value] : batch)
        {
            bool ok = false;
            try
            {
                ok = write(key.first, key.second, value.text);
            }
            catch (...)
            {
            }
            lock.lock();
            if (ok)
            {
                saved[key] = value.text;
            }
            else if (!stopping && !pending.contains(key) && ++value.attempts < 3)
            {
                if (pending.empty())
                {
                    first = std::chrono::steady_clock::now();
                }
                pending[key] = value;
                due = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            }
            lock.unlock();
        }
        lock.lock();
        writing = false;
    }
}
void StartConfigWriter()
{
    try
    {
        const auto s = Snapshot();
        if (!s.enabled || !s.pluginPath[0])
        {
            return;
        }
        writer = new ConfigWriter(WriteConfigValue);
    }
    catch (...)
    {
    }
}
void QueueConfig(const wchar_t* section, const wchar_t* key, const wchar_t* value) noexcept
{
    try
    {
        if (auto* p = writer.load())
        {
            p->Queue(section, key, value);
        }
    }
    catch (...)
    {
    }
}
void FlushConfig() noexcept
{
    try
    {
        if (auto* p = writer.load())
        {
            p->Flush();
        }
    }
    catch (...)
    {
    }
}
}
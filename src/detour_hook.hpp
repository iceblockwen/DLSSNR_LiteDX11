#pragma once
// Shared by the DXGI/loader hooks and the DX11 depth hooks: one Detours entry
// table per hooked signature, staged inside a single checked transaction.
#include <windows.h>
#include <intrin.h>
#include <tlhelp32.h>
#include <detours/detours.h>
#include <array>
#include <cstddef>
#include <utility>
#include <vector>

namespace lite::detour
{
inline void Check(LONG error)
{
    if (error != NO_ERROR)
    {
        throw HRESULT_FROM_WIN32(error);
    }
}
inline void* Method(void* object, size_t index)
{
    return (*static_cast<void***>(object))[index];
}

// Prepare all storage/handles before Detours suspends any peer. The handles
// must outlive Commit/Abort, which own suspension and instruction relocation.
class ThreadHandles
{
    std::vector<HANDLE> handles;

  public:
    ThreadHandles() = default;
    ThreadHandles(const ThreadHandles&) = delete;
    ThreadHandles& operator=(const ThreadHandles&) = delete;
    ~ThreadHandles()
    {
        for (auto handle : handles)
        {
            CloseHandle(handle);
        }
    }
    void Prepare()
    {
        const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            Check(GetLastError());
        }
        struct Snapshot
        {
            HANDLE value;
            ~Snapshot()
            {
                CloseHandle(value);
            }
        } owner{snapshot};
        const auto process = GetCurrentProcessId(), current = GetCurrentThreadId();
        std::vector<DWORD> ids;
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        BOOL more = Thread32First(snapshot, &entry);
        while (more)
        {
            if (entry.dwSize < offsetof(THREADENTRY32, th32OwnerProcessID) + sizeof(entry.th32OwnerProcessID))
            {
                Check(ERROR_BAD_LENGTH);
            }
            if (entry.th32OwnerProcessID == process && entry.th32ThreadID != current)
            {
                ids.push_back(entry.th32ThreadID);
            }
            entry.dwSize = sizeof(entry);
            more = Thread32Next(snapshot, &entry);
        }
        const auto enumerationError = GetLastError();
        if (enumerationError != ERROR_NO_MORE_FILES)
        {
            Check(enumerationError);
        }
        handles.reserve(ids.size());
        for (auto id : ids)
        {
            const auto handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
                                               THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                                           FALSE, id);
            if (!handle)
            {
                const auto error = GetLastError();
                if (error == ERROR_INVALID_PARAMETER)
                {
                    continue;
                }
                Check(error);
            }
            handles.push_back(handle);
            const auto ownerProcess = GetProcessIdOfThread(handle);
            if (!ownerProcess)
            {
                Check(GetLastError());
            }
            if (ownerProcess != process)
            {
                CloseHandle(handle);
                handles.pop_back();
            } // ID reused outside this process
        }
    }
    void Enlist()
    {
        for (auto handle : handles)
        {
            const auto state = WaitForSingleObject(handle, 0);
            if (state == WAIT_OBJECT_0)
            {
                continue; // exited since the snapshot
            }
            if (state == WAIT_FAILED)
            {
                Check(GetLastError());
            }
            if (state != WAIT_TIMEOUT)
            {
                Check(ERROR_INVALID_DATA);
            }
            Check(DetourUpdateThread(handle)); // any failure poisons the transaction: abort, never skip it
        }
    }
};

template <class Tag, class Result, class... Args>
struct Hook
{
    using Fn = Result(STDMETHODCALLTYPE*)(Args...);
    struct Entry
    {
        void* target{};
        Fn original{};
        bool installed{};
    };
    static inline std::array<Entry, 8> entries{};
    template <size_t N>
    static Result STDMETHODCALLTYPE Thunk(Args... args)
    {
        if constexpr (requires { Tag::CallFrom(entries[N].original, static_cast<void*>(nullptr), args...); })
        {
            return Tag::CallFrom(entries[N].original, _ReturnAddress(), args...);
        }
        else
        {
            return Tag::Call(entries[N].original, args...);
        }
    }
    template <size_t... N>
    static auto Thunks(std::index_sequence<N...>)
    {
        return std::array<Fn, sizeof...(N)>{Thunk<N>...};
    }
    static bool Installed(void* target)
    {
        for (const auto& e : entries)
        {
            if (e.installed && e.target == target)
            {
                return true;
            }
        }
        return false;
    }
    static void Stage(void* target)
    {
        for (const auto& e : entries)
        {
            if (e.target == target)
            {
                return;
            }
        }
        static const auto thunks = Thunks(std::make_index_sequence<8>{});
        for (size_t n = 0; n < entries.size(); ++n)
        {
            auto& e = entries[n];
            if (e.target)
            {
                continue;
            }
            e.target = target;
            e.original = reinterpret_cast<Fn>(target);
            Check(DetourAttach(reinterpret_cast<PVOID*>(&e.original), reinterpret_cast<PVOID>(thunks[n])));
            return;
        }
        throw E_OUTOFMEMORY;
    }
    static void Finish(bool committed)
    {
        for (auto& e : entries)
        {
            if (e.target && !e.installed)
            {
                if (committed)
                {
                    e.installed = true;
                }
                else
                {
                    e = {};
                }
            }
        }
    }
};

// Finish(success) marks staged entries of every hook family the caller owns.
template <void (*Finish)(bool)>
struct Transaction
{
    bool open = false;
    Transaction()
    {
        Check(DetourTransactionBegin());
        open = true;
        if (const auto error = DetourUpdateThread(GetCurrentThread()))
        {
            DetourTransactionAbort();
            open = false;
            Check(error);
        }
    }
    ~Transaction()
    {
        if (open)
        {
            DetourTransactionAbort();
            Finish(false);
        }
    }
    void Commit()
    {
        if (!open)
        {
            Check(ERROR_INVALID_OPERATION);
        }
        ThreadHandles threads;
        try
        {
            threads.Prepare();
            threads.Enlist();
        }
        catch (...)
        {
            DetourTransactionAbort();
            open = false;
            Finish(false);
            throw;
        }
        const auto error = DetourTransactionCommit();
        open = false;
        Finish(error == NO_ERROR);
        Check(error);
    }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
};
}

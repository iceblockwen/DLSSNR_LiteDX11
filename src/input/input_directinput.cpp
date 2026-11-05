// SPDX-License-Identifier: GPL-3.0-or-later
// Lite's state/data detours, checked against Opt input_system_directinput.cpp.
// ANSI/Unicode mouse/keyboard implementations may have different target entries.
#include "input.hpp"
#include "input_keyboard.hpp"
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <wrl/client.h>
#include <array>
#include <map>
#include <deque>
#include <mutex>
#include <cstring>
#include <algorithm>
namespace
{
using Device = IDirectInputDevice8W;
using StateFn = HRESULT(STDMETHODCALLTYPE*)(Device*, DWORD, LPVOID);
using DataFn = HRESULT(STDMETHODCALLTYPE*)(Device*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
using ReleaseFn = ULONG(STDMETHODCALLTYPE*)(Device*);
std::recursive_mutex mutex;
struct Entry
{
    StateFn state{};
    DataFn data{};
    ReleaseFn release{};
    void* stateTarget{};
    void* dataTarget{};
    void* releaseTarget{};
};
std::array<Entry, 4> entries;
struct Tracked
{
    StateFn state{};
    DataFn data{};
    bool keyboard{};
    DWORD kind{};
    std::array<bool, 256> suppressed{};
    std::map<DWORD, DIDEVICEOBJECTDATA> down;
    std::deque<DIDEVICEOBJECTDATA> releases;
};
std::map<Device*, Tracked> devices;
DWORD Kind(Device* device)
{
    if (const auto it = devices.find(device); it != devices.end())
    {
        return it->second.kind;
    }
    DIDEVCAPS caps{};
    caps.dwSize = sizeof(caps);
    return SUCCEEDED(device->GetCapabilities(&caps)) ? GET_DIDEVICE_TYPE(caps.dwDevType) : 0;
}
bool Supported(DWORD kind)
{
    return kind == DI8DEVTYPE_MOUSE || kind == DI8DEVTYPE_KEYBOARD;
}
bool Track(Device* device, size_t slot, DWORD kind)
{
    if (lite::PanelInputInternal() || !Supported(kind))
    {
        return false;
    }
    if (devices.contains(device))
    {
        return true;
    }
    auto** table = *reinterpret_cast<void***>(device);
    // A borrowed device is safe to retain only while its final Release is observed.
    bool releaseObserved{};
    for (const auto& entry : entries)
    {
        releaseObserved |= entry.releaseTarget == table[2] && entry.release;
    }
    if (!releaseObserved)
    {
        return false;
    }
    auto& tracked = devices[device];
    tracked.keyboard = kind == DI8DEVTYPE_KEYBOARD;
    tracked.kind = kind;
    // state and data may share different combinations of implementation entries.
    tracked.state = entries[slot].state;
    tracked.data = entries[slot].data;
    for (const auto& entry : entries)
    {
        if (entry.stateTarget == table[9] && entry.state)
        {
            tracked.state = entry.state;
        }
        if (entry.dataTarget == table[10] && entry.data)
        {
            tracked.data = entry.data;
        }
    }
    return true;
}
int VirtualKey(DWORD scan)
{
    // Calls are serialized by the DI mutex. Refresh on the caller's layout change.
    static HKL layout{};
    static std::array<int, 256> mapping{};
    const auto current = GetKeyboardLayout(0);
    if (layout != current || !layout)
    {
        for (DWORD code = 0; code < 256; ++code)
        {
            mapping[code] = code == DIK_PAUSE ? VK_PAUSE : int(MapVirtualKeyExW((code & 0x7f) | ((code & 0x80) ? 0xe000 : 0), MAPVK_VSC_TO_VK_EX, current));
        }
        layout = current;
    }
    return scan < mapping.size() ? mapping[scan] : 0;
}
template <size_t Slot>
HRESULT STDMETHODCALLTYPE OnState(Device* device, DWORD bytes, LPVOID data)
{
    std::lock_guard lock(mutex);
    const auto kind = Kind(device);
    const bool tracked = Track(device, Slot, kind);
    const auto result = entries[Slot].state(device, bytes, data);
    if (FAILED(result) || !data || lite::PanelInputInternal() || !Supported(kind))
    {
        return result;
    }
    const bool blocked = lite::PanelGameInputBlocked();
    if (kind == DI8DEVTYPE_KEYBOARD && bytes == 256 && tracked)
    {
        auto* keys = static_cast<BYTE*>(data);
        auto& suppressed = devices[device].suppressed;
        // Clear completed physical releases even when this poll has no DOWN keys.
        const auto pollBlocked = lite::PanelKeyboardPollMask();
        for (DWORD scan = 0; scan < 256; ++scan)
        {
            if (!(keys[scan] & 0x80))
            {
                suppressed[scan] = false;
                continue;
            }
            if (blocked)
            {
                suppressed[scan] = true;
            }
            if (blocked || suppressed[scan])
            {
                keys[scan] = 0;
                continue;
            }
            const int key = VirtualKey(scan);
            if (key >= 0 && key < 256 && pollBlocked[key])
            {
                keys[scan] = 0;
            }
        }
    }
    else if (blocked)
    {
        std::memset(data, 0, bytes);
    }
    return result;
}
template <size_t Slot>
HRESULT STDMETHODCALLTYPE OnData(Device* device, DWORD bytes, LPDIDEVICEOBJECTDATA data, LPDWORD count, DWORD flags)
{
    std::lock_guard lock(mutex);
    const auto kind = Kind(device);
    const bool tracking = Track(device, Slot, kind);
    if (!tracking || !count)
    {
        return entries[Slot].data(device, bytes, data, count, flags);
    }
    auto& tracked = devices[device];
    const bool blocked = lite::PanelGameInputBlocked();
    const DWORD requested = *count;
    if (bytes != sizeof(DIDEVICEOBJECTDATA))
    {
        const auto result = entries[Slot].data(device, bytes, data, count, blocked ? flags & ~DIGDD_PEEK : flags);
        if (SUCCEEDED(result) && blocked)
        {
            *count = 0;
        }
        return result; // Custom/legacy formats retain HRESULT and are not reinterpreted.
    }
    const bool peek = (flags & DIGDD_PEEK) != 0;
    const DWORD pending = DWORD((std::min)(size_t(requested), tracked.releases.size()));
    DWORD realCount = requested - pending;
    const auto result = entries[Slot].data(device, bytes, data ? data + pending : nullptr, &realCount, blocked ? flags & ~DIGDD_PEEK : flags);
    if (FAILED(result))
    {
        *count = realCount;
        return result;
    }
    if (!data)
    {
        if (!peek)
        {
            for (DWORD i = 0; i < pending; ++i)
            {
                tracked.releases.pop_front();
            }
        }
        *count = (blocked ? 0 : realCount) + pending;
        return result;
    }
    for (DWORD i = 0; i < pending; ++i)
    {
        data[i] = tracked.releases[i];
    }
    if (!peek)
    {
        for (DWORD i = 0; i < pending; ++i)
        {
            tracked.releases.pop_front();
        }
    }
    DWORD written = pending;
    for (DWORD i = 0; i < realCount; ++i)
    {
        const auto event = data[pending + i];
        const bool up = (event.dwData & 0x80) == 0;
        const bool keyboardKey = tracked.keyboard && event.dwOfs < 256;
        const bool suppressed = keyboardKey && tracked.suppressed[event.dwOfs];
        // A blocked PEEK drains the physical queue, so commit its ownership too.
        if (keyboardKey && (!peek || blocked))
        {
            if (up)
            {
                tracked.suppressed[event.dwOfs] = false;
            }
            else if (blocked)
            {
                tracked.suppressed[event.dwOfs] = true;
            }
        }
        if (blocked)
        {
            if (!tracked.keyboard || !up || !tracked.down.contains(event.dwOfs))
            {
                continue;
            }
        }
        else if (keyboardKey)
        {
            if (suppressed && (!up || !tracked.down.contains(event.dwOfs)))
            {
                continue;
            }
            if (!up && lite::PanelKeyboardPollBlocked(VirtualKey(event.dwOfs)))
            {
                continue;
            }
        }
        data[written++] = event;
        if (!peek && tracked.keyboard)
        {
            if (up)
            {
                tracked.down.erase(event.dwOfs);
            }
            else
            {
                tracked.down[event.dwOfs] = event;
            }
        }
    }
    *count = written;
    return result;
}
template <size_t Slot>
ULONG STDMETHODCALLTYPE OnRelease(Device* device)
{
    std::lock_guard lock(mutex);
    const auto result = entries[Slot].release(device);
    if (!result)
    {
        devices.erase(device);
    }
    return result;
}
template <size_t Slot>
void Probe(Device* device)
{
    auto** table = *reinterpret_cast<void***>(device);
    auto& entry = entries[Slot];
    bool stateExists{}, dataExists{}, releaseExists{};
    for (const auto& other : entries)
    {
        stateExists |= other.stateTarget == table[9];
        dataExists |= other.dataTarget == table[10];
        releaseExists |= other.releaseTarget == table[2];
    }
    if (!stateExists)
    {
        entry.state = reinterpret_cast<StateFn>(table[9]);
        if (lite::AttachPanelInputHook(reinterpret_cast<PVOID*>(&entry.state), reinterpret_cast<PVOID>(OnState<Slot>)))
        {
            entry.stateTarget = table[9];
        }
    }
    if (!dataExists)
    {
        entry.data = reinterpret_cast<DataFn>(table[10]);
        if (lite::AttachPanelInputHook(reinterpret_cast<PVOID*>(&entry.data), reinterpret_cast<PVOID>(OnData<Slot>)))
        {
            entry.dataTarget = table[10];
        }
    }
    if (!releaseExists)
    {
        entry.release = reinterpret_cast<ReleaseFn>(table[2]);
        if (lite::AttachPanelInputHook(reinterpret_cast<PVOID*>(&entry.release), reinterpret_cast<PVOID>(OnRelease<Slot>)))
        {
            entry.releaseTarget = table[2];
        }
    }
}
}
namespace lite
{
void FlushPanelDirectInput()
{
    std::lock_guard lock(mutex);
    for (auto& [device, tracked] : devices)
    {
        // Capture held keys even when the engine never polls while the menu is
        // open. DI-only keys must remain neutral after close until physical UP.
        if (tracked.keyboard && tracked.state)
        {
            BYTE keys[256]{};
            if (SUCCEEDED(tracked.state(device, sizeof(keys), keys)))
            {
                for (size_t scan = 0; scan < 256; ++scan)
                {
                    tracked.suppressed[scan] = (keys[scan] & 0x80) != 0;
                }
            }
        }
        // Only keyboard releases already owed to the engine are synthesized.
        // No press generated in the menu may survive in a buffered device.
        for (auto& [offset, event] : tracked.down)
        {
            (void)offset;
            event.dwData = 0;
            event.dwTimeStamp = GetTickCount();
            ++event.dwSequence;
            tracked.releases.push_back(event);
        }
        tracked.down.clear();
        if (tracked.data)
        {
            DWORD discard = INFINITE;
            tracked.data(device, sizeof(DIDEVICEOBJECTDATA), nullptr, &discard, 0);
        }
    }
}
void InstallPanelDirectInput()
{
    std::lock_guard lock(mutex);
    PanelInputScope bypass;
    Microsoft::WRL::ComPtr<IDirectInput8W> wide;
    Microsoft::WRL::ComPtr<IDirectInput8A> ansi;
    const auto module = GetModuleHandleW(nullptr);
    if (SUCCEEDED(DirectInput8Create(module, DIRECTINPUT_VERSION, IID_IDirectInput8W, reinterpret_cast<void**>(wide.GetAddressOf()), nullptr)))
    {
        Microsoft::WRL::ComPtr<IDirectInputDevice8W> mouse, keyboard;
        if (SUCCEEDED(wide->CreateDevice(GUID_SysMouse, &mouse, nullptr)))
        {
            Probe<0>(mouse.Get());
        }
        if (SUCCEEDED(wide->CreateDevice(GUID_SysKeyboard, &keyboard, nullptr)))
        {
            Probe<1>(keyboard.Get());
        }
    }
    if (SUCCEEDED(DirectInput8Create(module, DIRECTINPUT_VERSION, IID_IDirectInput8A, reinterpret_cast<void**>(ansi.GetAddressOf()), nullptr)))
    {
        Microsoft::WRL::ComPtr<IDirectInputDevice8A> mouse, keyboard;
        if (SUCCEEDED(ansi->CreateDevice(GUID_SysMouse, &mouse, nullptr)))
        {
            Probe<2>(reinterpret_cast<Device*>(mouse.Get()));
        }
        if (SUCCEEDED(ansi->CreateDevice(GUID_SysKeyboard, &keyboard, nullptr)))
        {
            Probe<3>(reinterpret_cast<Device*>(keyboard.Get()));
        }
    }
}
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Release ownership checked against Opt input_system_messages.cpp and
// ReShade input_windows.cpp. Lite's input hooks remain the interception base.
#include "input_keyboard.hpp"
#include "input.hpp"
#include <array>
#include <atomic>
#include <mutex>

namespace
{
std::mutex mutex;
struct Keys
{
    std::array<bool, 256> legacyDown{}, legacyBlocked{}, rawDown{}, rawBlocked{}, pollRelease{};
} keys;
// pollRelease is written only under mutex, so a reader outside the lock can skip
// the lock entirely while the menu owns no release at all. The count is a
// conservative upper bound of owned entries: a stale value only costs a lock.
std::atomic<unsigned> ownedReleases{};
void Own(size_t code, bool value)
{
    if (keys.pollRelease[code] == value)
    {
        return;
    }
    keys.pollRelease[code] = value;
    if (value)
    {
        ownedReleases.fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
        ownedReleases.fetch_sub(1, std::memory_order_relaxed);
    }
}
bool IsDown(UINT message)
{
    return message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
}
bool IsUp(UINT message)
{
    return message == WM_KEYUP || message == WM_SYSKEYUP;
}
bool IsText(UINT message)
{
    return message == WM_CHAR || message == WM_SYSCHAR || message == WM_DEADCHAR || message == WM_SYSDEADCHAR || message == WM_UNICHAR;
}
int KeyCode(WPARAM key, LPARAM param)
{
    if (key >= 256)
    {
        return -1;
    }
    if (key == VK_SHIFT)
    {
        const auto side = MapVirtualKeyW((UINT(param) >> 16) & 0xff, MAPVK_VSC_TO_VK_EX);
        return side ? int(side) : int(key);
    }
    if (key == VK_CONTROL)
    {
        return (param & (1LL << 24)) ? VK_RCONTROL : VK_LCONTROL;
    }
    if (key == VK_MENU)
    {
        return (param & (1LL << 24)) ? VK_RMENU : VK_LMENU;
    }
    return int(key);
}
bool SystemShortcut(UINT message, WPARAM key)
{
    return (message == WM_SYSKEYDOWN || message == WM_SYSKEYUP) && (key == VK_TAB || key == VK_F4);
}
bool PollHeldLocked(int key)
{
    if (!keys.pollRelease[key])
    {
        return false;
    }
    lite::PanelInputScope bypass;
    if (!(GetAsyncKeyState(key) & 0x8000))
    {
        Own(key, false);
    }
    return keys.pollRelease[key];
}
}
namespace lite
{
bool PanelKeyboardQueue(HWND window, UINT message, WPARAM key, LPARAM param)
{
    if (!IsDown(message) && !IsUp(message) && !IsText(message))
    {
        return false;
    }
    if (SystemShortcut(message, key))
    {
        return false;
    }
    if ((IsDown(message) || IsUp(message)) && key == VK_INSERT)
    {
        return true;
    }
    if (IsText(message))
    {
        return PanelInputCaptured(window);
    }
    const int code = KeyCode(key, param);
    if (code < 0 || code >= 256)
    {
        return false;
    }
    std::lock_guard lock(mutex);
    if (IsUp(message))
    {
        return keys.legacyBlocked[code];
    }
    return PanelInputCaptured(window) || ((param & (1LL << 30)) && keys.legacyBlocked[code]);
}
bool PanelKeyboardMessage(HWND window, UINT message, WPARAM key, LPARAM param)
{
    if (SystemShortcut(message, key))
    {
        return false;
    }
    const bool captured = PanelInputCaptured(window);
    if (IsText(message))
    {
        return captured;
    }
    if (!IsDown(message) && !IsUp(message))
    {
        return false;
    }
    const int code = KeyCode(key, param);
    if (code < 0 || code >= 256)
    {
        return false;
    }
    std::lock_guard lock(mutex);
    bool result{};
    if (IsDown(message))
    {
        // A repeat after opening is not a new press. Its eventual release still
        // belongs to the game if the original press was delivered there.
        if (!(param & (1LL << 30)))
        {
            keys.legacyBlocked[code] = captured || key == VK_INSERT;
            Own(code, captured);
        }
        keys.legacyDown[code] = true;
        result = captured || keys.legacyBlocked[code] || key == VK_INSERT;
    }
    else
    {
        result = keys.legacyBlocked[code] || key == VK_INSERT;
        keys.legacyDown[code] = keys.legacyBlocked[code] = false;
        Own(code, false);
    }
    return result;
}
bool PanelKeyboardPollBlocked(int key)
{
    if (PanelInputInternal() || key < 0 || key >= 256)
    {
        return false;
    }
    if (key == VK_INSERT)
    {
        return true; // Menu hotkey belongs to Lite in every input path.
    }
    if (PanelGameInputBlocked())
    {
        return true;
    }
    // An idle menu owns nothing, so the common case never takes the lock.
    if (!ownedReleases.load(std::memory_order_relaxed))
    {
        return false;
    }
    std::lock_guard lock(mutex);
    if (key == VK_SHIFT)
    {
        return PollHeldLocked(VK_LSHIFT) || PollHeldLocked(VK_RSHIFT);
    }
    if (key == VK_CONTROL)
    {
        return PollHeldLocked(VK_LCONTROL) || PollHeldLocked(VK_RCONTROL);
    }
    if (key == VK_MENU)
    {
        return PollHeldLocked(VK_LMENU) || PollHeldLocked(VK_RMENU);
    }
    return PollHeldLocked(key);
}
std::array<bool, 256> PanelKeyboardPollMask()
{
    std::array<bool, 256> blocked{};
    if (PanelInputInternal())
    {
        return blocked;
    }
    if (PanelGameInputBlocked())
    {
        blocked.fill(true);
        return blocked;
    }
    // Same short circuit as PanelKeyboardPollBlocked: no owned release, no mask.
    if (!ownedReleases.load(std::memory_order_relaxed))
    {
        blocked[VK_INSERT] = true;
        return blocked;
    }
    std::lock_guard lock(mutex);
    // Query only releases still owned by the menu, once per physical key.
    for (int key = 0; key < 256; ++key)
    {
        if (keys.pollRelease[key])
        {
            blocked[key] = PollHeldLocked(key);
        }
    }
    blocked[VK_SHIFT] = blocked[VK_LSHIFT] || blocked[VK_RSHIFT];
    blocked[VK_CONTROL] = blocked[VK_LCONTROL] || blocked[VK_RCONTROL];
    blocked[VK_MENU] = blocked[VK_LMENU] || blocked[VK_RMENU];
    blocked[VK_INSERT] = true;
    return blocked;
}
void PanelKeyboardTransition(bool enabled)
{
    if (!enabled)
    {
        return; // Do not discard ownership of releases still in flight.
    }
    std::lock_guard lock(mutex);
    // Keep event ownership: async physical state can lag behind a queued raw
    // event, and resetting rawDown here would misclassify a game-owned repeat.
    PanelInputScope bypass;
    for (int key = 0; key < 256; ++key)
    {
        if (!(GetAsyncKeyState(key) & 0x8000))
        {
            Own(key, false);
        }
    }
}
void PanelKeyboardFocusLost()
{
    std::lock_guard lock(mutex);
    // The UI state is cleared separately. Keep release ownership across focus
    // changes: clearing rawBlocked would leak an unmatched menu BREAK to games.
    PanelInputScope bypass;
    for (int key = 0; key < 256; ++key)
    {
        if (!(GetAsyncKeyState(key) & 0x8000))
        {
            Own(key, false);
        }
    }
}
bool PanelKeyboardOwnsReleases()
{
    return ownedReleases.load(std::memory_order_relaxed) != 0;
}
bool PanelRawKeyboardMessage(const RAWKEYBOARD& key)
{
    if (!key.VKey || key.VKey >= 255 || key.MakeCode == KEYBOARD_OVERRUN_MAKE_CODE)
    {
        return true;
    }
    RAWINPUT copy{};
    copy.header.dwType = RIM_TYPEKEYBOARD;
    copy.data.keyboard = key;
    FilterPanelRawInput(&copy, sizeof(copy));
    return copy.data.keyboard.VKey == 0;
}
void FilterPanelRawInput(RAWINPUT* input, unsigned bytes)
{
    if (!input || PanelInputInternal() || bytes < sizeof(RAWINPUTHEADER))
    {
        return;
    }
    if (input->header.dwType == RIM_TYPEMOUSE)
    {
        if (PanelGameInputBlocked() && bytes >= offsetof(RAWINPUT, data) + sizeof(RAWMOUSE))
        {
            input->data.mouse = {};
        }
        return;
    }
    if (input->header.dwType != RIM_TYPEKEYBOARD || bytes < offsetof(RAWINPUT, data) + sizeof(RAWKEYBOARD))
    {
        return;
    }
    auto& key = input->data.keyboard;
    if (!key.VKey || key.VKey >= 255 || key.MakeCode == KEYBOARD_OVERRUN_MAKE_CODE)
    {
        return;
    }
    std::lock_guard lock(mutex);
    const int code = KeyCode(key.VKey, (LPARAM(key.MakeCode & 0xff) << 16) | ((key.Flags & RI_KEY_E0) ? (1LL << 24) : 0));
    const bool up = (key.Flags & RI_KEY_BREAK) != 0;
    bool blocked{};
    if (!up)
    {
        if (!keys.rawDown[code])
        {
            keys.rawBlocked[code] = PanelGameInputBlocked() || code == VK_INSERT;
            Own(code, keys.rawBlocked[code]);
        }
        keys.rawDown[code] = true;
        blocked = PanelGameInputBlocked() || keys.rawBlocked[code];
    }
    else
    {
        keys.rawDown[code] = false;
        Own(code, false);
        // Keep this bit until the next MAKE: the same HRAWINPUT can be read by
        // both the engine's queue loop and WndProc, and must filter identically.
        blocked = keys.rawBlocked[code];
    }
    if (blocked)
    {
        input->header.hDevice = nullptr;
        input->header.wParam = RIM_INPUTSINK;
        key.Flags |= RI_KEY_BREAK;
        key.VKey = 0;
        key.Message = WM_KEYUP;
    }
}
}

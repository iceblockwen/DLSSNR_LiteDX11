// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from DLSSNR_Lite_old/src/input.cpp; cursor defer/restore also checked
// against Opt's input_system_cursor.cpp and ReShade's input_windows.cpp.
#include "input.hpp"
#include "input_keyboard.hpp"
#include "../graphics.hpp"
#include <detours/detours.h>
#include <imgui.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <wrl/client.h>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <vector>
#include <windowsx.h>
#include <imgui_impl_win32.h>
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace
{
using QWORD = unsigned long long;
std::atomic<HWND> owner{};
thread_local bool internal{};
std::mutex cursorMutex;
RECT deferredClip{};
bool haveClip{}, rawOnly{}, pointerEvent{}, buttonEvents{};
bool rawKeyboardOnly{};
POINT askedCursor{};
decltype(&ClipCursor) realClip = ClipCursor;
decltype(&SetCursorPos) realSetPos = SetCursorPos;
decltype(&GetCursorPos) realGetPos = GetCursorPos;
decltype(&GetAsyncKeyState) realAsync = GetAsyncKeyState;
decltype(&GetKeyState) realKey = GetKeyState;
decltype(&GetKeyboardState) realKeyboard = GetKeyboardState;
decltype(&PeekMessageA) realPeekA = PeekMessageA;
decltype(&PeekMessageW) realPeekW = PeekMessageW;
decltype(&GetMessageA) realMessageA = GetMessageA;
decltype(&GetMessageW) realMessageW = GetMessageW;
std::atomic<lite::PanelQueueRoute> queueRoute{};
thread_local bool readingQueue{};
struct QueueScope
{
    bool before = readingQueue;
    QueueScope()
    {
        readingQueue = true;
    }
    ~QueueScope()
    {
        readingQueue = before;
    }
};
void RouteQueue(MSG* message, bool removed)
{
    if (!message || internal)
    {
        return;
    }
    if (const auto route = queueRoute.load())
    {
        route(*message, removed);
    }
}
BOOL WINAPI OnPeekA(LPMSG msg, HWND window, UINT first, UINT last, UINT flags)
{
    if (readingQueue || internal)
    {
        return realPeekA(msg, window, first, last, flags);
    }
    QueueScope scope;
    const auto result = realPeekA(msg, window, first, last, flags);
    if (result)
    {
        RouteQueue(msg, (flags & PM_REMOVE) != 0);
    }
    return result;
}
BOOL WINAPI OnPeekW(LPMSG msg, HWND window, UINT first, UINT last, UINT flags)
{
    if (readingQueue || internal)
    {
        return realPeekW(msg, window, first, last, flags);
    }
    QueueScope scope;
    const auto result = realPeekW(msg, window, first, last, flags);
    if (result)
    {
        RouteQueue(msg, (flags & PM_REMOVE) != 0);
    }
    return result;
}
BOOL WINAPI OnMessageA(LPMSG msg, HWND window, UINT first, UINT last)
{
    if (readingQueue || internal)
    {
        return realMessageA(msg, window, first, last);
    }
    QueueScope scope;
    const auto result = realMessageA(msg, window, first, last);
    if (result > 0)
    {
        RouteQueue(msg, true);
    }
    return result;
}
BOOL WINAPI OnMessageW(LPMSG msg, HWND window, UINT first, UINT last)
{
    if (readingQueue || internal)
    {
        return realMessageW(msg, window, first, last);
    }
    QueueScope scope;
    const auto result = realMessageW(msg, window, first, last);
    if (result > 0)
    {
        RouteQueue(msg, true);
    }
    return result;
}
decltype(&GetRawInputData) realRaw = GetRawInputData;
decltype(&GetRawInputBuffer) realBuffer = GetRawInputBuffer;
decltype(&RegisterRawInputDevices) realRegisterRaw = RegisterRawInputDevices;
decltype(&PostMessageA) realPostA = PostMessageA;
decltype(&PostMessageW) realPostW = PostMessageW;
decltype(&ShowCursor) realShow = ShowCursor;
decltype(&SetCursor) realCursor = SetCursor;

// ShowCursor and SetCursor belong to the window's input thread. In particular,
// Present/teardown on a rendering thread must never change that thread's counter.
// A temporary, process-local message hook finishes asynchronous restoration even
// after the panel subclass/context has been destroyed. It never takes GraphicsMutex.
std::mutex nativeMutex;
struct NativeCursor
{
    HWND requested{}, active{};
    HWND priorCapture{};
    DWORD thread{};
    HHOOK messages{};
    HCURSOR gameCursor{}, panelCursor{};
    int gameCount{}, actualCount{};
    bool held{}, failed{};
} native;
bool nativeHooks{};
struct CaptureRestore
{
    HWND active{}, prior{};
};
thread_local CaptureRestore captureRestore;
// Capture changes synchronously enter the window procedure. Never do that while
// holding nativeMutex: the render thread takes GraphicsMutex then nativeMutex.
class NativeLock
{
    std::unique_lock<std::mutex> lock{nativeMutex};

  public:
    ~NativeLock()
    {
        const auto restore = captureRestore;
        captureRestore = {};
        lock.unlock();
        if (!restore.active)
        {
            return;
        }
        if (GetCapture() == restore.active && restore.prior != restore.active)
        {
            ReleaseCapture();
        }
        else if (restore.prior == restore.active && GetCapture() != restore.active &&
                 GetForegroundWindow() == restore.active)
        {
            SetCapture(restore.active);
        }
    }
};
UINT CursorWake()
{
    static const UINT message = RegisterWindowMessageW(L"Lite.NativeCursor.Sync.1");
    return message;
}
LRESULT CALLBACK NativeMessages(int code, WPARAM wparam, LPARAM lparam);
void RestoreNativeLocked()
{
    if (!native.active)
    {
        return;
    }
    if (native.held)
    {
        while (native.actualCount < native.gameCount)
        {
            native.actualCount = realShow(TRUE);
        }
        while (native.actualCount > native.gameCount)
        {
            native.actualCount = realShow(FALSE);
        }
    }
    realCursor(native.gameCursor);
    captureRestore = {native.active, native.priorCapture};
    native.held = false;
}
void SyncNativeLocked()
{
    if (!native.thread || GetCurrentThreadId() != native.thread)
    {
        return;
    }
    const DWORD requestedThread = native.requested ? GetWindowThreadProcessId(native.requested, nullptr) : 0;
    if (native.requested && requestedThread != native.thread)
    {
        RestoreNativeLocked();
        if (native.messages)
        {
            UnhookWindowsHookEx(native.messages);
        }
        const HWND next = native.requested;
        native = {};
        native.requested = next;
        native.thread = requestedThread;
        realPostW(next, CursorWake(), 0, 0);
        return;
    }
    if (native.active != native.requested)
    {
        RestoreNativeLocked();
        native.active = native.requested;
        native.failed = false;
        native.gameCursor = GetCursor();
        native.priorCapture = GetCapture();
    }
    if (!native.requested)
    {
        if (native.messages)
        {
            UnhookWindowsHookEx(native.messages);
        }
        native = {};
        return;
    }
    if (!native.messages && !native.failed)
    {
        if (nativeHooks)
        {
            native.messages = SetWindowsHookExW(WH_GETMESSAGE, NativeMessages, nullptr, native.thread);
        }
        if (!native.messages)
        {
            native.failed = true;
        }
    }
    if (!native.held && !native.failed)
    {
        native.panelCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!native.panelCursor)
        {
            native.failed = true;
            return;
        }
        native.actualCount = realShow(TRUE);
        native.gameCount = native.actualCount - 1;
        // Bound acquisition for broken/unusually deep hide loops; restore every
        // probe increment and use the software fallback instead of stalling.
        for (int i = 0; native.actualCount < 0 && i < 64; ++i)
        {
            native.actualCount = realShow(TRUE);
        }
        while (native.actualCount > (std::max)(0, native.gameCount))
        {
            native.actualCount = realShow(FALSE);
        }
        if (native.actualCount < 0)
        {
            while (native.actualCount > native.gameCount)
            {
                native.actualCount = realShow(FALSE);
            }
            native.failed = true;
            return;
        }
        realCursor(native.panelCursor);
        native.held = true;
    }
    if (native.held)
    {
        realCursor(native.panelCursor);
    }
}
LRESULT CALLBACK NativeMessages(int code, WPARAM wparam, LPARAM lparam)
{
    if (code == HC_ACTION && wparam == PM_REMOVE)
    {
        {
            NativeLock lock;
            SyncNativeLocked();
        }
        auto* message = reinterpret_cast<MSG*>(lparam);
        if (message->message == CursorWake())
        {
            message->message = WM_NULL;
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}
void CaptureNative(HWND window)
{
    NativeLock lock;
    native.requested = window;
    if (window && !native.thread)
    {
        native.thread = GetWindowThreadProcessId(window, nullptr);
    }
    if (GetCurrentThreadId() == native.thread)
    {
        SyncNativeLocked();
    }
    // Install on the GUI thread itself: a hook must not depend on the lifetime
    // of a temporary rendering thread that happened to request acquisition.
    else if (native.messages)
    {
        PostThreadMessageW(native.thread, CursorWake(), 0, 0);
    }
    else if (window)
    {
        realPostW(window, CursorWake(), 0, 0);
    }
}
int WINAPI OnShow(BOOL show)
{
    NativeLock lock;
    SyncNativeLocked();
    if (native.held && native.thread == GetCurrentThreadId() && !internal)
    {
        return native.gameCount += show ? 1 : -1;
    }
    const int result = realShow(show);
    if (native.held && native.thread == GetCurrentThreadId())
    {
        native.actualCount = result;
    }
    return result;
}
HCURSOR WINAPI OnCursor(HCURSOR cursor)
{
    NativeLock lock;
    SyncNativeLocked();
    if (native.requested && internal && native.thread)
    {
        if (cursor)
        {
            native.panelCursor = cursor;
        }
        if (GetCurrentThreadId() == native.thread)
        {
            return realCursor(native.panelCursor);
        }
        PostThreadMessageW(native.thread, CursorWake(), 0, 0);
        return GetCursor();
    }
    if (native.held && native.thread == GetCurrentThreadId())
    {
        const auto previous = native.gameCursor;
        native.gameCursor = cursor;
        realCursor(native.panelCursor);
        return previous;
    }
    if (!internal && native.active && native.thread == GetCurrentThreadId())
    {
        native.gameCursor = cursor;
    }
    return realCursor(cursor);
}
bool Block()
{
    return owner.load() && !internal;
}
BOOL WINAPI OnClip(const RECT* rect)
{
    if (Block())
    {
        std::lock_guard lock(cursorMutex);
        haveClip = rect != nullptr;
        if (rect)
        {
            deferredClip = *rect;
        }
        return TRUE;
    }
    return realClip(rect);
}
BOOL WINAPI OnSetPos(int x, int y)
{
    {
        std::lock_guard lock(cursorMutex);
        askedCursor = {x, y};
    }
    return Block() ? TRUE : realSetPos(x, y);
}
BOOL WINAPI OnGetPos(LPPOINT point)
{
    if (Block() && point)
    {
        std::lock_guard lock(cursorMutex);
        *point = askedCursor;
        return TRUE;
    }
    // CapturePanelInput() re-reads the physical position when it starts, so a
    // position observed while the panel is closed is never worth a lock.
    return realGetPos(point);
}
SHORT WINAPI OnAsync(int key)
{
    return lite::PanelKeyboardPollBlocked(key) ? 0 : realAsync(key);
}
SHORT WINAPI OnKey(int key)
{
    return lite::PanelKeyboardPollBlocked(key) ? 0 : realKey(key);
}
BOOL WINAPI OnKeyboard(PBYTE keys)
{
    const BOOL result = realKeyboard(keys);
    if (result && keys && !internal)
    {
        const auto blocked = lite::PanelKeyboardPollMask();
        for (int key = 0; key < 256; ++key)
        {
            if (blocked[key])
            {
                keys[key] = 0;
            }
        }
    }
    return result;
}
UINT WINAPI OnRaw(HRAWINPUT handle, UINT command, LPVOID data, PUINT size, UINT header)
{
    const auto result = realRaw(handle, command, data, size, header);
    if (command == RID_INPUT && data && result != UINT(-1))
    {
        lite::FilterPanelRawInput(static_cast<RAWINPUT*>(data), result);
    }
    return result;
}
UINT WINAPI OnBuffer(PRAWINPUT data, PUINT size, UINT header)
{
    const auto result = realBuffer(data, size, header);
    if (data && result != UINT(-1))
    {
        for (UINT i = 0; i < result; ++i)
        {
            lite::FilterPanelRawInput(data, data->header.dwSize);
            data = NEXTRAWINPUTBLOCK(data);
        }
    }
    return result;
}
// The reserved hotkey is only reachable through raw input when the game took the
// keyboard away from the message queue. That registration is cached here: one
// query per RegisterRawInputDevices call instead of three syscalls per packet.
std::atomic<UINT_PTR> rawKeyboardTarget{};
void RefreshRawKeyboardTarget()
{
    UINT_PTR target{};
    UINT count{};
    if (GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE)) != UINT(-1) && count)
    {
        std::vector<RAWINPUTDEVICE> devices(count);
        if (GetRegisteredRawInputDevices(devices.data(), &count, sizeof(RAWINPUTDEVICE)) != UINT(-1))
        {
            for (const auto& device : devices)
            {
                if (device.usUsagePage == 1 && device.usUsage == 6 &&
                    (device.dwFlags & RIDEV_NOLEGACY) == RIDEV_NOLEGACY)
                {
                    if (!device.hwndTarget)
                    {
                        target = 1; // every window
                        break;
                    }
                    if (!target)
                    {
                        target = reinterpret_cast<UINT_PTR>(device.hwndTarget);
                    }
                }
            }
        }
    }
    rawKeyboardTarget.store(target, std::memory_order_relaxed);
}
BOOL WINAPI OnRegisterRaw(PCRAWINPUTDEVICE devices, UINT count, UINT size)
{
    const BOOL result = realRegisterRaw(devices, count, size);
    if (result)
    {
        RefreshRawKeyboardTarget();
    }
    return result;
}
BOOL WINAPI OnPostA(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (Block() && window == owner.load() && message == WM_MOUSEMOVE)
    {
        return TRUE;
    }
    return realPostA(window, message, wparam, lparam);
}
BOOL WINAPI OnPostW(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (Block() && window == owner.load() && message == WM_MOUSEMOVE)
    {
        return TRUE;
    }
    return realPostW(window, message, wparam, lparam);
}
bool Attach(PVOID* original, PVOID detour)
{
    if (!*original)
    {
        return false;
    }
    LONG error = DetourTransactionBegin();
    if (error == NO_ERROR)
    {
        error = DetourUpdateThread(GetCurrentThread());
        if (error == NO_ERROR)
        {
            error = DetourAttach(original, detour);
        }
        if (error == NO_ERROR)
        {
            error = DetourTransactionCommit();
        }
        else
        {
            DetourTransactionAbort();
        }
    }
    return error == NO_ERROR;
}
}
namespace lite
{
bool PanelGameInputBlocked()
{
    return Block();
}
bool PanelInputInternal()
{
    return internal;
}
bool AttachPanelInputHook(PVOID* original, PVOID hook)
{
    return Attach(original, hook);
}
PanelInputScope::PanelInputScope() : previous(internal)
{
    internal = true;
}
PanelInputScope::~PanelInputScope()
{
    internal = previous;
}
void InstallPanelInput(PanelQueueRoute route)
{
    if (route)
    {
        queueRoute = route;
    }
    static std::once_flag once;
    std::call_once(once, []
                   {
                       struct HookPair
                       {
                           PVOID* original;
                           PVOID detour;
                       };
                       const HookPair pairs[]{
                           {reinterpret_cast<PVOID*>(&realClip), reinterpret_cast<PVOID>(OnClip)},
                           {reinterpret_cast<PVOID*>(&realSetPos), reinterpret_cast<PVOID>(OnSetPos)},
                           {reinterpret_cast<PVOID*>(&realGetPos), reinterpret_cast<PVOID>(OnGetPos)},
                           {reinterpret_cast<PVOID*>(&realAsync), reinterpret_cast<PVOID>(OnAsync)},
                           {reinterpret_cast<PVOID*>(&realKey), reinterpret_cast<PVOID>(OnKey)},
                           {reinterpret_cast<PVOID*>(&realKeyboard), reinterpret_cast<PVOID>(OnKeyboard)},
                           {reinterpret_cast<PVOID*>(&realPeekA), reinterpret_cast<PVOID>(OnPeekA)},
                           {reinterpret_cast<PVOID*>(&realPeekW), reinterpret_cast<PVOID>(OnPeekW)},
                           {reinterpret_cast<PVOID*>(&realMessageA), reinterpret_cast<PVOID>(OnMessageA)},
                           {reinterpret_cast<PVOID*>(&realMessageW), reinterpret_cast<PVOID>(OnMessageW)},
                           {reinterpret_cast<PVOID*>(&realRaw), reinterpret_cast<PVOID>(OnRaw)},
                           {reinterpret_cast<PVOID*>(&realBuffer), reinterpret_cast<PVOID>(OnBuffer)},
                           {reinterpret_cast<PVOID*>(&realRegisterRaw), reinterpret_cast<PVOID>(OnRegisterRaw)},
                           {reinterpret_cast<PVOID*>(&realPostA), reinterpret_cast<PVOID>(OnPostA)},
                           {reinterpret_cast<PVOID*>(&realPostW), reinterpret_cast<PVOID>(OnPostW)},
                       };
                       for (const auto& pair : pairs)
                       {
                           (void)Attach(pair.original, pair.detour);
                       }

                       const bool showReady = Attach(reinterpret_cast<PVOID*>(&realShow), reinterpret_cast<PVOID>(OnShow));
                       const bool shapeReady = Attach(reinterpret_cast<PVOID*>(&realCursor), reinterpret_cast<PVOID>(OnCursor));
                       nativeHooks = showReady && shapeReady;
                       // A game that registered raw devices before this install is covered here.
                       RefreshRawKeyboardTarget();
                       InstallPanelDirectInput();
                   });
}
bool PanelRawHotkeyWatched(HWND window)
{
    const auto target = rawKeyboardTarget.load(std::memory_order_relaxed);
    return target == 1 || (target && target == reinterpret_cast<UINT_PTR>(window));
}
bool PanelInputCaptured(HWND window)
{
    return window && owner.load() == window;
}
bool PanelNativeCursor(HWND window)
{
    std::lock_guard lock(nativeMutex);
    return window && native.requested == window && native.active == window && native.held;
}
bool PanelCursorMessage(UINT message)
{
    if (message != CursorWake())
    {
        return false;
    }
    NativeLock lock;
    SyncNativeLocked();
    return true;
}
void CapturePanelInput(HWND window, bool enabled)
{
    if (!window)
    {
        return;
    }
    {
        std::lock_guard lock(cursorMutex);
        if (enabled)
        {
            if (owner.load() == window)
            {
                return;
            }
            if (!window || owner.load())
            {
                return;
            }
            haveClip = GetClipCursor(&deferredClip) != FALSE;
            realGetPos(&askedCursor);
            pointerEvent = buttonEvents = false;
            rawOnly = rawKeyboardOnly = false;
            UINT count{};
            if (GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE)) != UINT(-1) && count)
            {
                std::vector<RAWINPUTDEVICE> devices(count);
                if (GetRegisteredRawInputDevices(devices.data(), &count, sizeof(RAWINPUTDEVICE)) != UINT(-1))
                {
                    for (const auto& device : devices)
                    {
                        if (device.usUsagePage == 1 &&
                            (device.dwFlags & RIDEV_NOLEGACY) == RIDEV_NOLEGACY && (!device.hwndTarget || device.hwndTarget == window))
                        {
                            if (device.usUsage == 2)
                            {
                                rawOnly = true;
                            }
                            if (device.usUsage == 6)
                            {
                                rawKeyboardOnly = true;
                            }
                        }
                    }
                }
            }
            owner = window;
            realClip(nullptr);
        }
        else
        {
            if (owner.load() != window)
            {
                return;
            }
            FlushPanelDirectInput();
            owner = nullptr;
            pointerEvent = buttonEvents = false;
            // Never re-lock another application after Alt+Tab/window destruction.
            realClip(haveClip && GetForegroundWindow() == window ? &deferredClip : nullptr);
        }
        if (enabled)
        {
            FlushPanelDirectInput();
        }
    }
    PanelKeyboardTransition(enabled);
    if (enabled)
    {
        CaptureNative(window); // Snapshot game capture before backend cleanup.
    }
    // Prime/release once at transitions. Never append a polling button state to
    // a frame that already contains legacy/raw button messages.
    if (ImGui::GetCurrentContext())
    {
        auto& io = ImGui::GetIO();
        io.ClearEventsQueue();
        io.ClearInputKeys();
        io.ClearInputMouse();
        io.AddFocusEvent(enabled);
        // Reset backend button ownership as well as ImGui's current state. GUI
        // thread restoration handles ReleaseCapture when this runs from Present.
        {
            PanelInputScope bypass;
            for (UINT message : {WM_LBUTTONUP, WM_RBUTTONUP, WM_MBUTTONUP})
            {
                ImGui_ImplWin32_WndProcHandler(window, message, 0, 0);
            }
            for (WORD button : {WORD(XBUTTON1), WORD(XBUTTON2)})
            {
                ImGui_ImplWin32_WndProcHandler(window, WM_XBUTTONUP, MAKEWPARAM(0, button), 0);
            }
        }
        const int keys[] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
        for (int i = 0; i < 5; ++i)
        {
            ImGui::GetIO().AddMouseButtonEvent(i, enabled && (realAsync(keys[i]) & 0x8000));
        }
    }
    // Backend UP cleanup can call ReleaseCapture. Restore game-owned capture
    // only after that cleanup, on the GUI thread that owns native cursor state.
    if (!enabled)
    {
        CaptureNative(nullptr);
    }
}
void PanelMouseMessage(HWND window, UINT message, LPARAM position)
{
    if (!PanelInputCaptured(window))
    {
        return;
    }
    const bool client = message >= WM_MOUSEFIRST && message <= WM_MOUSELAST;
    if (!client && message != WM_NCMOUSEMOVE)
    {
        return;
    }
    POINT point{GET_X_LPARAM(position), GET_Y_LPARAM(position)};
    if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL || message == WM_NCMOUSEMOVE)
    {
        if (!ScreenToClient(window, &point))
        {
            return;
        }
    }
    ImGui::GetIO().AddMousePosEvent(float(point.x), float(point.y));
    pointerEvent = true;
    if (message != WM_MOUSEMOVE && message != WM_MOUSEWHEEL && message != WM_MOUSEHWHEEL && message != WM_NCMOUSEMOVE)
    {
        buttonEvents = true;
    }
}
bool PanelRawInput(HWND window, HRAWINPUT handle)
{
    if (!PanelInputCaptured(window))
    {
        return false;
    }
    RAWINPUT input{};
    UINT bytes = sizeof(input);
    const auto result = realRaw(handle, RID_INPUT, &input, &bytes, sizeof(RAWINPUTHEADER));
    if (result == UINT(-1) || result < sizeof(RAWINPUTHEADER))
    {
        return false;
    }
    if (input.header.dwType == RIM_TYPEKEYBOARD && result >= offsetof(RAWINPUT, data) + sizeof(RAWKEYBOARD))
    {
        const auto& key = input.data.keyboard;
        const bool consumed = PanelRawKeyboardMessage(key);
        if (rawKeyboardOnly && key.VKey && key.VKey < 255 && key.MakeCode != KEYBOARD_OVERRUN_MAKE_CODE)
        {
            const bool up = (key.Flags & RI_KEY_BREAK) != 0;
            const LPARAM position = (LPARAM(key.MakeCode & 0xff) << 16) | ((key.Flags & RI_KEY_E0) ? (1LL << 24) : 0) | (up ? (3LL << 30) : 0) | 1;
            PanelInputScope bypass;
            ImGui_ImplWin32_WndProcHandler(window, up ? WM_KEYUP : WM_KEYDOWN, key.VKey, position);
            if (!up)
            {
                BYTE keyboard[256]{};
                realKeyboard(keyboard);
                for (int vk = 0; vk < 256; ++vk)
                {
                    keyboard[vk] = BYTE((keyboard[vk] & 1) | ((realAsync(vk) & 0x8000) ? 0x80 : 0));
                }
                wchar_t text[8]{};
                const int count = ToUnicodeEx(key.VKey, key.MakeCode, keyboard, text, 8, 4, GetKeyboardLayout(0));
                for (int i = 0; i < count; ++i)
                {
                    ImGui::GetIO().AddInputCharacterUTF16(text[i]);
                }
            }
        }
        return consumed;
    }
    if (input.header.dwType != RIM_TYPEMOUSE || result < offsetof(RAWINPUT, data) + sizeof(RAWMOUSE))
    {
        return false;
    }
    // Legacy registrations already provide button/wheel messages. Do not duplicate them.
    if (!rawOnly)
    {
        return true;
    }
    auto& io = ImGui::GetIO();
    // Raw movement has neither Windows pointer speed nor acceleration applied.
    // Use the OS position associated with this queued message, BEFORE its buttons.
    const DWORD position = GetMessagePos();
    POINT point{GET_X_LPARAM(position), GET_Y_LPARAM(position)};
    if (ScreenToClient(window, &point))
    {
        io.AddMousePosEvent(float(point.x), float(point.y));
        pointerEvent = true;
    }
    const USHORT down[] = {RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_5_DOWN};
    const USHORT up[] = {RI_MOUSE_LEFT_BUTTON_UP, RI_MOUSE_RIGHT_BUTTON_UP, RI_MOUSE_MIDDLE_BUTTON_UP, RI_MOUSE_BUTTON_4_UP, RI_MOUSE_BUTTON_5_UP};
    for (int i = 0; i < 5; ++i)
    {
        if (input.data.mouse.usButtonFlags & down[i])
        {
            io.AddMouseButtonEvent(i, true);
            buttonEvents = true;
        }
        if (input.data.mouse.usButtonFlags & up[i])
        {
            io.AddMouseButtonEvent(i, false);
            buttonEvents = true;
        }
    }
    if (input.data.mouse.usButtonFlags & RI_MOUSE_WHEEL)
    {
        io.AddMouseWheelEvent(0, static_cast<SHORT>(input.data.mouse.usButtonData) / float(WHEEL_DELTA));
    }
    if (input.data.mouse.usButtonFlags & RI_MOUSE_HWHEEL)
    {
        io.AddMouseWheelEvent(-static_cast<SHORT>(input.data.mouse.usButtonData) / float(WHEEL_DELTA), 0);
    }
    return true;
}
bool PanelRawHotkey(HWND window, HRAWINPUT handle, UINT& message)
{
    // Query only for the reserved hotkey. A keyboard registered NOLEGACY must
    // still be able to open a hidden panel; never toggle twice in legacy mode.
    if (!PanelRawHotkeyWatched(window))
    {
        return false;
    }
    RAWINPUT input{};
    UINT bytes = sizeof(input);
    const auto result = realRaw(handle, RID_INPUT, &input, &bytes, sizeof(RAWINPUTHEADER));
    if (result == UINT(-1) || result < offsetof(RAWINPUT, data) + sizeof(RAWKEYBOARD) ||
        input.header.dwType != RIM_TYPEKEYBOARD || input.data.keyboard.VKey != VK_INSERT)
    {
        return false;
    }
    message = (input.data.keyboard.Flags & RI_KEY_BREAK) ? WM_KEYUP : WM_KEYDOWN;
    return true;
}
void UpdatePanelPointer(HWND window)
{
    auto& io = ImGui::GetIO();
    const bool captured = PanelInputCaptured(window);
    io.MouseDrawCursor = captured && !PanelNativeCursor(window);
    if (!captured)
    {
        return;
    }
    if (!pointerEvent)
    {
        POINT point{};
        if (realGetPos(&point) && ScreenToClient(window, &point))
        {
            io.AddMousePosEvent(float(point.x), float(point.y));
        }
    }
    pointerEvent = false;
    // Only for a window that supplies no button messages at all. Once a legacy
    // or raw button source exists, its queued ordering is authoritative.
    if (!buttonEvents)
    {
        const int keys[] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
        for (int i = 0; i < 5; ++i)
        {
            io.AddMouseButtonEvent(i, (realAsync(keys[i]) & 0x8000) != 0);
        }
    }
}
}

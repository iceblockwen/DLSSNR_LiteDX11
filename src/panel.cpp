#include "panel_impl.hpp"
#include "ui_widgets.hpp"
#include "input.hpp"
#include "input_keyboard.hpp"
#include "nr/depth11.hpp"
#include "config_writer.hpp"
#include "config_storage.hpp"
#include <imgui.h>
#include <imgui_impl_win32.h>

#include <map>
#include <algorithm>
#include <mutex>
#include <memory>
#include <stdexcept>
#include <cstdio>
#include <cstring>
#include <string>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
using Microsoft::WRL::ComPtr;
using namespace lite::panel_ui;

// The shell: construction, window subclass, teardown, and the public Panel methods.
// The Direct3D 11 renderer is in panel_gpu.cpp, the ImGui window in panel_ui.cpp.
namespace lite {
using panel_detail::Check;

namespace {
bool WindowFocused(HWND window) {
GUITHREADINFO info{sizeof(info)};
return window && GetGUIThreadInfo(GetWindowThreadProcessId(window, nullptr), &info) &&
       info.hwndFocus == window;
}
// Process-lifetime registry avoids GPU work during DLL destruction under loader lock.
struct Registry {
std::recursive_mutex mutex;
std::map<IDXGISwapChain *, std::shared_ptr<lite::Panel>> panels;
};
Registry &Global() {
static auto *registry = new Registry;
return *registry;
}
}

std::recursive_mutex &GraphicsMutex() {
return Global().mutex;
}

void Panel::Impl::SyncPreviewDemand() {
    const bool visible = status.visible && hwnd && status.panel != State::Failed;
    if (previewDemand == visible)
        return;
    nr::SetPanelVisible(this, visible);
    previewDemand = visible;
    if (!visible) {
        CommitTuning();
        CommitFlowOptions();
        if (scaleEdited) {
            nr::SetDisplayScale(nr::DisplayScale(), true);
            scaleEdited = false;
        }
        FlushConfig();
    }
}

Panel::Impl::~Impl() {
    Unsubclass();
    DestroyRenderer();
    if (device11)
        nr::ReleaseNativeCapture(device11.Get());
    nr::RetireCompleted();
    if (context) {
        ImGui::SetCurrentContext(context);
        if (win32Ready)
            ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(context);
    }
}

bool Panel::Impl::RouteKeyboard(MSG &message, bool removed) {
    const UINT kind = message.message;
    if (kind != WM_KEYDOWN && kind != WM_KEYUP && kind != WM_SYSKEYDOWN &&
        kind != WM_SYSKEYUP && kind != WM_CHAR && kind != WM_SYSCHAR &&
        kind != WM_DEADCHAR && kind != WM_SYSDEADCHAR && kind != WM_UNICHAR)
        return false;
    std::unique_lock lock(Global().mutex);
    const auto route = Windows().Find(message.hwnd);
    if (!route || !route->Active() ||
        !PanelKeyboardQueue(message.hwnd, message.message, message.wParam, message.lParam))
        return false;
    // WindowProc revalidates the route and owns its own lock. Do not keep
    // an outer recursive acquisition across its native callback.
    lock.unlock();
    if (removed) {
        // Translate before replacing the engine's copy, otherwise WM_CHAR is
        // lost when a game examines its queue before calling DispatchMessage.
        if (message.wParam != VK_INSERT)
            TranslateMessage(&message);
        WindowProc(message.hwnd, message.message, message.wParam, message.lParam);
    }
    message.message = WM_NULL;
    message.wParam = 0;
    message.lParam = 0;
    return true;
}

LRESULT CALLBACK Panel::Impl::WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (PanelCursorMessage(message))
        return 0;
    std::unique_lock lock(Global().mutex);
    auto *route = Windows().Find(window);
    if (!route) {
        lock.unlock();
        return DefWindowProcW(window, message, wparam, lparam);
    }
    auto old = route->original;
    auto *self = route->Active();
    if (!self) {
        lock.unlock();
        const auto result = CallWindowProcW(old, window, message, wparam, lparam);
        if (message == WM_NCDESTROY) {
            lock.lock();
            Windows().Erase(window);
        }
        return result;
    }
    if (self->context)
        ImGui::SetCurrentContext(self->context);
    if (message == WM_INPUT) {
        UINT hotkey{};
        if (PanelRawHotkey(window, reinterpret_cast<HRAWINPUT>(lparam), hotkey)) {
            WindowProc(window, hotkey, VK_INSERT, 0);
            return DefWindowProcW(window, message, wparam, lparam);
        }
    }
    if (message == WM_KILLFOCUS || (message == WM_ACTIVATEAPP && !wparam)) {
        self->inputFocused = false;
        self->ignoreMenuRelease = true;
        CapturePanelInput(window, false);
        PanelKeyboardFocusLost();
    }
    if (message == WM_SETFOCUS || (message == WM_ACTIVATEAPP && wparam)) {
        GUITHREADINFO info{sizeof(info)};
        self->inputFocused =
            message == WM_SETFOCUS ||
            (GetGUIThreadInfo(GetWindowThreadProcessId(window, nullptr), &info) &&
             info.hwndFocus == window);
        if (self->status.visible)
            CapturePanelInput(window, self->inputFocused && GetForegroundWindow() == window);
    }
    if ((message == WM_KEYDOWN || message == WM_KEYUP || message == WM_SYSKEYDOWN ||
         message == WM_SYSKEYUP) &&
        wparam == VK_INSERT) {
        PanelKeyboardMessage(window, message, wparam, lparam);
        if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN)
            self->ignoreMenuRelease = false;
        if (message == WM_KEYUP || message == WM_SYSKEYUP) {
            if (self->ignoreMenuRelease) {
                self->ignoreMenuRelease = false;
                return 0;
            }
            self->status.visible = !self->status.visible;
            self->inputFocused = WindowFocused(window);
            self->SyncPreviewDemand();
            CapturePanelInput(window, self->status.visible && self->inputFocused &&
                                      GetForegroundWindow() == window);
        }
        return 0;
    }
    const bool keyboardBlocked = PanelKeyboardMessage(window, message, wparam, lparam);
    if (self->context && self->status.visible && PanelInputCaptured(window)) {
        ImGui::SetCurrentContext(self->context);
        if (message == WM_INPUT && PanelInputCaptured(window)) {
            if (PanelRawInput(window, reinterpret_cast<HRAWINPUT>(lparam)))
                return DefWindowProcW(window, message, wparam, lparam);
            // A keyboard release owed to the game must reach its original
            // input handler even if the panel opened after the press.
        }
        PanelMouseMessage(window, message, lparam);
        LRESULT handled{};
        {
            PanelInputScope internalInput;
            handled = ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam);
        }
        if (message == WM_SETCURSOR && PanelInputCaptured(window) && handled)
            return handled;
        if (PanelInputCaptured(window) && message >= WM_MOUSEFIRST && message <= WM_MOUSELAST)
            return 0;
    }
    if (keyboardBlocked)
        return 0;
    if (message == WM_NCDESTROY) {
        try {
            self->DestroyRenderer();
        } catch (...) {
            self->Failure(E_FAIL);
        }
        while (auto *remaining = Windows().Find(window)) {
            auto *owner = remaining->Active();
            if (!owner) break;
            owner->Unsubclass();
            owner->hwnd = nullptr;
            owner->status.visible = false;
        }
    }
    // The game may wait for its render thread. Keep only the native callback
    // value and do not access self/route after dropping the graphics lock.
    lock.unlock();
    const auto result = CallWindowProcW(old, window, message, wparam, lparam);
    if (message == WM_NCDESTROY) {
        lock.lock();
        Windows().Erase(window);
    }
    return result;
}

void Panel::Impl::Unsubclass() {
    status.visible = false;
    SyncPreviewDemand();
    if (context)
        ImGui::SetCurrentContext(context);
    if (!hwnd || !windowAttached)
        return;
    const auto route = Windows().Find(hwnd);
    if (route && route->Active() == this) CapturePanelInput(hwnd, false);
    Windows().Detach(hwnd, this, WindowProc);
    windowAttached = false;
}

Panel::Panel(IDXGISwapChain *swapchain, ID3D11Device *device)
: impl(std::make_unique<Impl>()) {
auto &p = *impl;
p.swap = swapchain;
DXGI_SWAP_CHAIN_DESC desc{};
Check(swapchain->GetDesc(&desc));
p.hwnd = desc.OutputWindow;
GUITHREADINFO info{sizeof(info)};
p.inputFocused = GetGUIThreadInfo(GetWindowThreadProcessId(p.hwnd, nullptr), &info) &&
                 info.hwndFocus == p.hwnd;
p.status.width = desc.BufferDesc.Width;
p.status.height = desc.BufferDesc.Height;
p.device11 = device;
if (!p.device11)
    throw E_NOINTERFACE;
p.device11->GetImmediateContext(&p.immediate);
p.capturing = nr::NativeCaptureEnabled();
try {
    if (p.capturing)
        nr::depth::Install(p.device11.Get(), p.immediate.Get());
} catch (...) {
    Log(L"DX11 depth tracking installation failed; panel and original rendering retained");
}
p.status.graphics = State::Ready;
if (!p.hwnd)
    throw E_NOTIMPL;
p.context = ImGui::CreateContext();
ImGui::SetCurrentContext(p.context);
auto &io = ImGui::GetIO();
io.IniFilename = nullptr;
io.LogFilename = nullptr;
io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
ImGui::StyleColorsDark();
auto &style = ImGui::GetStyle();
style.WindowRounding = 7;
style.FrameRounding = 4;
style.WindowPadding = ImVec2(18, 14);
style.ItemSpacing = ImVec2(10, 8);
p.baseStyle = style;
const float scale = ImGui_ImplWin32_GetDpiScaleForHwnd(p.hwnd);
p.dpiScale = p.fontDpiScale = scale;
style.ScaleAllSizes(scale);
style.MouseCursorScale = 1.f;
wchar_t windows[MAX_PATH]{};
GetWindowsDirectoryW(windows, MAX_PATH);
const std::wstring fontPath = std::wstring(windows) + L"\\Fonts\\segoeui.ttf";
char utf8[MAX_PATH * 3]{};
WideCharToMultiByte(CP_UTF8, 0, fontPath.c_str(), -1, utf8, sizeof(utf8), nullptr, nullptr);
if (!io.Fonts->AddFontFromFileTTF(utf8, 14 * scale)) {
    io.Fonts->AddFontDefault();
    io.FontGlobalScale = scale;
    p.fontDpiScale = 1;
}
if (!ImGui_ImplWin32_Init(p.hwnd))
    throw E_FAIL;
p.win32Ready = true;
Check(Impl::Windows().Attach(p.hwnd, &p, Impl::WindowProc));
p.windowAttached = true;
InstallPanelInput(Impl::RouteKeyboard);
p.inputFocused = WindowFocused(p.hwnd);
}
Panel::~Panel() {
Close();
}
void Panel::Close() {
if (!impl)
    return;
impl->Unsubclass();
impl.reset();
}
void Panel::Draw(UINT flags) noexcept {
auto &p = *impl;
if (!p.hwnd || (flags & DXGI_PRESENT_TEST))
    return;
try {
    nr::RetireCompleted();
    nr::NativeFrame11(p.swap, p.immediate.Get());
} catch (...) {
}
if (!p.status.visible || p.status.panel == State::Failed)
    return;
try {
    PanelInputScope internalInput;
    ImGui::SetCurrentContext(p.context);
    p.inputFocused = WindowFocused(p.hwnd);
    CapturePanelInput(p.hwnd, p.inputFocused && GetForegroundWindow() == p.hwnd);
    const float scale = ImGui_ImplWin32_GetDpiScaleForHwnd(p.hwnd);
    if (scale != p.dpiScale) {
        ImGui::GetStyle() = p.baseStyle;
        ImGui::GetStyle().ScaleAllSizes(scale);
        ImGui::GetStyle().MouseCursorScale = 1.f;
        p.dpiScale = scale;
        ImGui::GetIO().FontGlobalScale = scale / p.fontDpiScale;
    }
    p.renderStage=L"renderer initialization";
    if (!p.rendererReady)
        p.InitRenderer();
    p.renderStage=L"DX11 panel draw";
    p.Draw11();
} catch (HRESULT hr) {
    p.Failure(hr);
} catch (...) {
    p.Failure(E_FAIL);
}
}
void Panel::BeforeResize() {
auto &p = *impl;
p.renderStage=L"swapchain resize preparation";
p.DestroyRenderer();
nr::RetireCompleted();
nr::ReleaseNativeCapture(p.device11.Get());
}
void Panel::AfterResize(HRESULT hr) {
auto &p = *impl;
p.renderStage=L"swapchain resize completion";
DXGI_SWAP_CHAIN_DESC desc{};
if (SUCCEEDED(p.swap->GetDesc(&desc))) {
    p.status.width = desc.BufferDesc.Width;
    p.status.height = desc.BufferDesc.Height;
}
if (p.device11 && p.capturing)
    nr::depth::Register(p.immediate.Get());
// Failed ResizeBuffers still leaves the old buffers usable; rebuild on the next visible frame.
p.status.panel = State::Waiting;
p.status.error = hr;
}
void Panel::Fail(HRESULT error) noexcept {
impl->Failure(error);
}
void TrackSwapchain(IDXGISwapChain *swap, IUnknown *device) noexcept {
try {
    ComPtr<ID3D11Device> device11;
    if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&device11))))
        return;
    std::lock_guard lock(Global().mutex);
    if (Global().panels.contains(swap))
        return;
    auto panel = std::make_shared<Panel>(swap, device11.Get());
    Global().panels.emplace(swap, std::move(panel));
} catch (HRESULT hr) {
    GraphicsFailure(hr, L"Unsupported swapchain, window or command queue");
} catch (...) {
    GraphicsFailure(E_FAIL, L"Cannot attach DLSSNR_LiteDX11 panel");
}
}
void DrawSwapchain(IDXGISwapChain *swap, UINT flags) noexcept {
try {
    std::lock_guard lock(Global().mutex);
    const auto it = Global().panels.find(swap);
    if (it != Global().panels.end())
        it->second->Draw(flags);
} catch (...) {
}
}
HRESULT ResizeSwapchain(IDXGISwapChain *swap, bool before, HRESULT result) noexcept {
try {
    std::lock_guard lock(Global().mutex);
    const auto it = Global().panels.find(swap);
    if (it == Global().panels.end())
        return S_OK;
    if (before)
        it->second->BeforeResize();
    else
        it->second->AfterResize(result);
    return S_OK;
} catch (HRESULT hr) {
    FailSwapchain(swap, hr);
    return hr;
} catch (...) {
    FailSwapchain(swap, E_FAIL);
    return E_FAIL;
}
}
void FailSwapchain(IDXGISwapChain *swap, HRESULT error) noexcept {
try {
    std::lock_guard lock(Global().mutex);
    const auto it = Global().panels.find(swap);
    if (it != Global().panels.end())
        it->second->Fail(error);
} catch (...) {
}
}
void ReleaseSwapchain(IUnknown *swap) noexcept {
try {
    std::lock_guard lock(Global().mutex);
    const auto it = Global().panels.find(reinterpret_cast<IDXGISwapChain *>(swap));
    if (it != Global().panels.end()) {
        auto panel = std::move(it->second);
        Global().panels.erase(it);
        panel->Close();
    }
} catch (...) {
}
}
void GraphicsFailure(HRESULT hr, const wchar_t *reason) noexcept {
try {
    wchar_t detail[640]{};
    swprintf_s(detail,L"Graphics failure: %s HRESULT=0x%08X",reason,unsigned(hr));
    Log(detail);
} catch (...) {
}
}
}
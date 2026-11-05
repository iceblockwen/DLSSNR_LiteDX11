// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>
namespace lite {
// Extracted from Lite's input.cpp. The ImGui caller bypasses game-facing hooks.
using PanelQueueRoute = bool(*)(MSG&, bool removed);
void InstallPanelInput(PanelQueueRoute route = nullptr);
bool PanelGameInputBlocked();
bool PanelInputInternal();
bool AttachPanelInputHook(PVOID* original, PVOID hook);
void InstallPanelDirectInput();
void FlushPanelDirectInput();
void CapturePanelInput(HWND window, bool enabled);
bool PanelInputCaptured(HWND window);
bool PanelNativeCursor(HWND window);
// Called before the panel registry lock; services render-thread requests on GUI.
bool PanelCursorMessage(UINT message);
// Submit a legacy event's position before its button/wheel event in the backend.
void PanelMouseMessage(HWND window, UINT message, LPARAM position);
bool PanelRawInput(HWND window, HRAWINPUT input);
bool PanelRawHotkey(HWND window, HRAWINPUT input, UINT& message);
void UpdatePanelPointer(HWND window);
class PanelInputScope {
    bool previous;
public:
    PanelInputScope();
    ~PanelInputScope();
};
}

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>
#include <array>
namespace lite
{
// Opt/ReShade distinguish a press consumed by the menu from a release owed to
// the game. Polling and raw paths retain separate release state.
bool PanelKeyboardMessage(HWND, UINT, WPARAM, LPARAM);
bool PanelKeyboardQueue(HWND, UINT, WPARAM, LPARAM);
bool PanelKeyboardPollBlocked(int key);
std::array<bool, 256> PanelKeyboardPollMask();
void PanelKeyboardTransition(bool enabled);
void PanelKeyboardFocusLost();
void FilterPanelRawInput(RAWINPUT* input, unsigned bytes);
bool PanelRawKeyboardMessage(const RAWKEYBOARD& key);
}

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "frame.hpp"
namespace lite::nr::depth
{
inline thread_local unsigned internal{};
struct Internal
{
    Internal()
    {
        ++internal;
    }
    ~Internal()
    {
        --internal;
    }
};
void Install(ID3D11Device*, ID3D11DeviceContext*);
// Manual override: keep the depth the last EndFrame reported, ignoring detection.
bool PinCurrent();
void ClearPin();
bool Pinned();
struct Result
{
    ComPtr<ID3D11Texture2D> image;
    bool inverted{}, deferred{};
    // Facts about the winner, for the panel. Unset when image is empty.
    unsigned width{}, height{}, format{}, candidates{};
    bool written{}; // the winner was bound with depth writes enabled this frame
};
// Lite restrack + ReShade clear preservation. No readback or CPU GPU wait.
void Register(ID3D11DeviceContext*);
// Per-context demand: hooks use a cached atomic policy, not a draw-time map lock.
bool TrackingEnabled(ID3D11DeviceContext*);
void SetGameGuides(ID3D11DeviceContext*, bool supplied);
// Binding scan: the last depth bound with depth writes enabled wins. No draw hooks.
void Binding(ID3D11DeviceContext*, ID3D11DepthStencilView*);
void Cleared(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT flags, float);
void State(ID3D11DeviceContext*, ID3D11DepthStencilState*);
void ClearState(ID3D11DeviceContext*);
void Finished(ID3D11DeviceContext*, ID3D11CommandList*, BOOL restore);
void Executed(ID3D11DeviceContext*, ID3D11CommandList*, BOOL restore);
// capture=false never consults the scan, but an explicit pin still applies.
Result EndFrame(ID3D11DeviceContext*, unsigned width, unsigned height, bool capture = true);
void Release(ID3D11Device*);
}

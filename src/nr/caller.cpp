// SPDX-License-Identifier: GPL-3.0-or-later
// Lite pass.cpp / Opt dlssnr/forwarder: calls must originate in nvngx.dll-containing
// module; a tail call would erase that frame and fail the NR caller check.
#include "contracts.hpp"
extern "C" __declspec(dllexport) __declspec(noinline) int __cdecl LiteNrCreate(lite::nr::CreateFn fn, ID3D12GraphicsCommandList* commands, int feature, const void* params, void** handle) {
    if (!fn) return int(E_POINTER);
    volatile int result = fn(commands, feature, params, handle); return result;
}
extern "C" __declspec(dllexport) __declspec(noinline) int __cdecl LiteNrEvaluate(lite::nr::EvaluateFn fn, ID3D12GraphicsCommandList* commands, const void* handle, const void* params, void* callback) {
    if (!fn) return int(E_POINTER);
    volatile int result = fn(commands, handle, params, callback); return result;
}
extern "C" __declspec(dllexport) __declspec(noinline) int __cdecl LiteNrRelease(lite::nr::ReleaseFn fn, void* handle) {
    if (!fn) return int(E_POINTER);
    volatile int result = fn(handle); return result;
}

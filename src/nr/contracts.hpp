#pragma once
#include <d3d12.h>
namespace lite::nr {
// Concrete frame, bridge and executor contracts: frame.hpp and pass.hpp.
using CreateFn = int(__cdecl*)(ID3D12GraphicsCommandList*, int, const void*, void**);
using EvaluateFn = int(__cdecl*)(ID3D12GraphicsCommandList*, const void*, const void*, void*);
using ReleaseFn = int(__cdecl*)(void*);
}

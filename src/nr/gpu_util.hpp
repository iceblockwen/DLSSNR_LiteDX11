// Shared Direct3D 12 helper: the root signature and compute pipeline layout every
// Lite compute shader uses. No plugin state.
#pragma once
#include <d3d12.h>
#include <cstddef>
namespace lite::nr
{
void ComputePipeline(ID3D12Device*, const D3D12_DESCRIPTOR_RANGE* ranges, unsigned rangeCount,
                     unsigned constantWords, const void* bytecode, size_t bytecodeSize,
                     ID3D12RootSignature** root, ID3D12PipelineState** pipeline);
}

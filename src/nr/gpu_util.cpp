#include "gpu_util.hpp"
#include "frame.hpp"
namespace lite::nr
{
// The layout every Lite compute shader uses: one descriptor table over `ranges`
// and, when `constantWords` is not zero, one 32-bit constant block at b0.
void ComputePipeline(ID3D12Device* device, const D3D12_DESCRIPTOR_RANGE* ranges, unsigned rangeCount,
                     unsigned constantWords, const void* bytecode, size_t bytecodeSize,
                     ID3D12RootSignature** root, ID3D12PipelineState** pipeline)
{
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable = {rangeCount, ranges};
    unsigned count = 1;
    if (constantWords)
    {
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[1].Constants = {0, 0, constantWords};
        count = 2;
    }
    if (!*root)
    {
        D3D12_ROOT_SIGNATURE_DESC desc{count, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> code, error;
        ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &code, &error));
        ThrowIfFailed(device->CreateRootSignature(0, code->GetBufferPointer(), code->GetBufferSize(),
                                                  IID_PPV_ARGS(root)));
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC state{};
    state.pRootSignature = *root;
    state.CS = {bytecode, bytecodeSize};
    ThrowIfFailed(device->CreateComputePipelineState(&state, IID_PPV_ARGS(pipeline)));
}
}

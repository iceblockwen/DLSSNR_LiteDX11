// SPDX-License-Identifier: GPL-3.0-or-later
#include "white_point.hpp"
#include "nr_shaders.hpp"
#include <algorithm>
#include <cmath>
#include <vector>
namespace lite::nr {
namespace {
bool Positive(float v){return std::isfinite(v)&&v>0;}
// Lite shaders/dlssnr.hlsl mode 4: bounded samples of a 64x64 tile lattice.
// Source is the unedited input; slot 0 remains reserved like Lite's exposure carrier.

}
void WhitePoint::Reset(){status={};started=measured=false;previous=WhiteSource::Waiting;}
void WhitePoint::Consume(std::span<const float> grid) {
    std::vector<float> tiles;tiles.reserve(grid.size());
    for(size_t n=1;n<grid.size();++n)if(std::isfinite(grid[n])&&grid[n]>=0)tiles.push_back(grid[n]);
    if(tiles.size()<16){measured=false;status.statisticsValid=false;return;}
    std::sort(tiles.begin(),tiles.end());
    auto at=[&](double q){return tiles[size_t(q*double(tiles.size()-1))];};
    status.p50=at(.5);status.p90=at(.9);status.p99=at(.99);status.peak=tiles.back();
    double sum=0,total=0;for(float v:tiles){sum+=std::log((std::max)(double(v),1e-12));total+=v;}
    status.mean=float(total/double(tiles.size()));status.logAverage=float(std::exp(sum/double(tiles.size())));measured=status.statisticsValid=true;
}
WhiteStatus WhitePoint::Resolve(const WhiteSettings& s,const Exposure& e,float dt,bool reset) {
    // WhiteSettings arrive validated by SetModelSettings; game exposure and dt are bounded here.
    if(reset || mode!=s.mode){Reset();mode=s.mode;}
    const float manual=s.manual;
    const float trim=s.trim;
    dt=Clamped(dt,0,10,0);
    auto direct=[&](WhiteSource source,float value){status.source=source;status.value=std::clamp(value,1e-8f,1024.f);status.target=status.value;status.valid=true;previous=source;started=false;return status;};
    if(!e.sceneLinear)return direct(WhiteSource::Display,manual);
    if(s.mode==WhiteMode::Manual)return direct(WhiteSource::Manual,manual);
    const float pre=s.usePreExposure?e.pre:1;
    const float scale=s.useExposureScale&&e.exposurePresent?e.scale:1;
    const bool game=(!s.usePreExposure||(e.prePresent&&Positive(pre)))&&Positive(scale);
    if(s.mode==WhiteMode::Game&&game) {
        const float ratio=s.reverseRatio?scale/pre:pre/scale;
        return direct(WhiteSource::Game,
                      std::pow((std::max)(ratio,1e-8f),s.exponent)*trim);
    }
    if(s.mode==WhiteMode::Game || !measured || !Positive(status.logAverage)) {
        status.source=status.valid?WhiteSource::Held:WhiteSource::Waiting;
        if(!status.valid)status.value=manual;status.target=status.value;return status;
    }
    status.source=WhiteSource::Measured;status.target=std::clamp(status.logAverage/s.anchor*trim,1e-8f,1024.f);
    if(previous!=WhiteSource::Measured){started=false;previous=WhiteSource::Measured;}
    if(!started){status.value=status.target;started=true;}
    else if(std::fabs(status.target/status.value-1)>=s.deadband) {
        float next=status.value+(status.target-status.value)*(1-std::exp(-dt/s.tau));
        next=std::clamp(next,status.value*std::exp2(-s.release*dt),status.value*std::exp2(s.attack*dt));
        status.value=std::clamp(next,1e-8f,1024.f);
    }
    status.valid=true;return status;
}
WhiteMeter12::WhiteMeter12(ID3D12Device* d):device(d) {
    D3D12_DESCRIPTOR_RANGE ranges[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,2}};
    D3D12_ROOT_PARAMETER p{};p.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;p.DescriptorTable={2,ranges};
    D3D12_ROOT_SIGNATURE_DESC desc{1,&p,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};ComPtr<ID3DBlob> code,error;
    ThrowIfFailed(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&code,&error));ThrowIfFailed(d->CreateRootSignature(0,code->GetBufferPointer(),code->GetBufferSize(),IID_PPV_ARGS(&root)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};ps.pRootSignature=root.Get();ps.CS={liteMeterShader,sizeof(liteMeterShader)};ThrowIfFailed(d->CreateComputePipelineState(&ps,IID_PPV_ARGS(&pipeline)));
}
HRESULT WhiteMeter12::Record(ID3D12GraphicsCommandList* list,ID3D12Resource* source,std::shared_ptr<WhiteReadback>& output,ID3D12Resource* exposure) {
    if(!list||!source)return E_POINTER;
    try {
        auto desc=source->GetDesc();ComPtr<ID3D12Device> owner,listOwner;
        ThrowIfFailed(source->GetDevice(IID_PPV_ARGS(&owner)));ThrowIfFailed(list->GetDevice(IID_PPV_ARGS(&listOwner)));
        if(owner.Get()!=device.Get()||listOwner.Get()!=device.Get())return E_INVALIDARG;
        if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||desc.SampleDesc.Count!=1||desc.DepthOrArraySize!=1||!desc.Width||!desc.Height||desc.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)return E_NOTIMPL;
        if(!ShaderViewCompatible(desc.Format))return E_NOTIMPL;
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{ViewFormat(desc.Format)};ThrowIfFailed(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support)));if(!(support.Support1&D3D12_FORMAT_SUPPORT1_SHADER_LOAD))return E_NOTIMPL;
        std::shared_ptr<WhiteReadback> recording;bool reused=false;
        for(auto& free:pool)if(free.use_count()==1){recording=free;reused=true;break;}
        if(!recording){if(pool.size()>=4)return S_FALSE;recording=std::make_shared<WhiteReadback>();}
        if(exposure){auto d=exposure->GetDesc();ComPtr<ID3D12Device> exposureOwner;ThrowIfFailed(exposure->GetDevice(IID_PPV_ARGS(&exposureOwner)));if(exposureOwner.Get()!=device.Get()||d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||d.Width!=1||d.Height!=1||d.DepthOrArraySize!=1||d.MipLevels!=1||d.SampleDesc.Count!=1||(d.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)||!ShaderViewCompatible(d.Format))exposure=nullptr;}
        recording->source=source;recording->exposure=exposure;recording->hasExposure=exposure!=nullptr;recording->root=root;recording->pipeline=pipeline;
        if(!reused){
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        desc={};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=desc.Height=64;desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R32_FLOAT;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ThrowIfFailed(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&recording->grid)));
        heap.Type=D3D12_HEAP_TYPE_READBACK;desc={};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=64*64*4;desc.Height=desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ThrowIfFailed(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&recording->readback)));
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,3,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};ThrowIfFailed(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&recording->heap)));
        pool.push_back(recording);
        }
        if(reused)Transition(list,recording->grid.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        auto cpu=recording->heap->GetCPUDescriptorHandleForHeapStart();D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=ViewFormat(source->GetDesc().Format);srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=1;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        device->CreateShaderResourceView(source,&srv,cpu);cpu.ptr+=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        srv.Format=exposure?ViewFormat(exposure->GetDesc().Format):DXGI_FORMAT_R32_FLOAT;device->CreateShaderResourceView(exposure,&srv,cpu);cpu.ptr+=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=DXGI_FORMAT_R32_FLOAT;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;device->CreateUnorderedAccessView(recording->grid.Get(),nullptr,&uav,cpu);
        auto* descriptors=recording->heap.Get();list->SetDescriptorHeaps(1,&descriptors);list->SetComputeRootSignature(root.Get());list->SetPipelineState(pipeline.Get());list->SetComputeRootDescriptorTable(0,recording->heap->GetGPUDescriptorHandleForHeapStart());list->Dispatch(8,8,1);
        Transition(list,recording->grid.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=recording->grid.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=recording->readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint.Footprint={DXGI_FORMAT_R32_FLOAT,64,64,1,256};
        list->CopyTextureRegion(&to,0,0,0,&from,nullptr);output=std::move(recording);return S_OK;
    }catch(HRESULT hr){return hr;}
}
HRESULT WhiteReadback::Consume(WhitePoint& white,float* gameExposure) const {
    if(!readback)return E_POINTER;void* data{};D3D12_RANGE read{0,64*64*4};auto hr=readback->Map(0,&read,&data);if(FAILED(hr))return hr;
    try {if(gameExposure)*gameExposure=hasExposure?*static_cast<const float*>(data):0;white.Consume(std::span(static_cast<const float*>(data),64*64));}
    catch(...) {D3D12_RANGE written{};readback->Unmap(0,&written);return E_OUTOFMEMORY;}
    D3D12_RANGE written{};readback->Unmap(0,&written);return S_OK;
}
}

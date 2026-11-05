// SPDX-License-Identifier: GPL-3.0-or-later
// Extracted mechanism: Lite bridge.cpp (shared twins, same adapter, GPU fences,
// depth/MV conversion, fence retirement). Opt with_dx12 sync checked alongside it.
#include "frame.hpp"
#include "../graphics.hpp"
#include <dxgi1_6.h>
#include "nr_shaders.hpp"
#include <vector>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
namespace lite::nr {
namespace {
struct Handle {
    HANDLE value{}; bool owned{};
    ~Handle(){ if(value && owned) CloseHandle(value); }
};
bool CopyCompatible(DXGI_FORMAT a,DXGI_FORMAT b) {
    return ViewFormat(a)==ViewFormat(b);
}
bool Plain(const D3D11_TEXTURE2D_DESC& d) {
    return d.Width && d.Height && d.ArraySize==1 && d.MipLevels==1 && d.SampleDesc.Count==1;
}
struct Twin {
    ComPtr<ID3D11Texture2D> texture, backup;
    ComPtr<ID3D12Resource> resource;
    ComPtr<ID3D11ShaderResourceView> view;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ComPtr<ID3D11Resource> source;
    std::shared_ptr<Handle> handle;
    D3D11_TEXTURE2D_DESC desc{};
    unsigned outWidth{}, outHeight{}; // shared twin size; desc is the source size
    bool converted{};
};
// Only the touched compute bindings change. Backup sampling avoids unbinding a
// game's depth target or disturbing its OM UAVs.
struct ComputeState {
    ID3D11DeviceContext* context;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ID3D11ClassInstance* classes[256]{}; UINT count=256;
    explicit ComputeState(ID3D11DeviceContext* c):context(c) {
        c->CSGetShader(&shader,classes,&count); c->CSGetShaderResources(0,1,&srv); c->CSGetUnorderedAccessViews(0,1,&uav);
    }
    ~ComputeState() {
        ID3D11ShaderResourceView* emptySrv{}; ID3D11UnorderedAccessView* emptyUav{};
        UINT keep=UINT(-1); context->CSSetShaderResources(0,1,&emptySrv); context->CSSetUnorderedAccessViews(0,1,&emptyUav,&keep);
        auto* s=srv.Get(); auto* u=uav.Get();
        context->CSSetShader(shader.Get(),classes,count); context->CSSetShaderResources(0,1,&s);
        context->CSSetUnorderedAccessViews(0,1,&u,&keep);
        for(UINT n=0;n<count;++n) if(classes[n]) classes[n]->Release();
    }
};
struct Recording {
    ID3D12GraphicsCommandList* list;
    bool open=true;
    HRESULT Close() { open=false; return list->Close(); }
    ~Recording() { if(open) list->Close(); }
};

struct BridgeTransport {
    ComPtr<ID3D11Device5> device11;
    ComPtr<ID3D11DeviceContext4> context;
    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D11Fence> fence11;
    ComPtr<ID3D12Fence> fence12;
    // Serialize complete hand-offs, not just counter allocation: both APIs signal
    // this fence. A later signal must never bypass an earlier GPU dependency.
    std::recursive_mutex mutex;
    UINT64 value{};
    bool failed{};
    explicit BridgeTransport(ID3D11DeviceContext* input) {
        ThrowIfFailed(input->QueryInterface(IID_PPV_ARGS(&context)));
        ComPtr<ID3D11Device> base;input->GetDevice(&base);ThrowIfFailed(base.As(&device11));
        ComPtr<IDXGIDevice> dxgi;ThrowIfFailed(base.As(&dxgi));ComPtr<IDXGIAdapter> adapter;ThrowIfFailed(dxgi->GetAdapter(&adapter));
        ThrowIfFailed(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device12)));
        D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        ThrowIfFailed(device12->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));
        ThrowIfFailed(device11->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&fence11)));
        Handle shared;shared.owned=true;
        ThrowIfFailed(fence11->CreateSharedHandle(nullptr,GENERIC_ALL,nullptr,&shared.value));
        ThrowIfFailed(device12->OpenSharedHandle(shared.value,IID_PPV_ARGS(&fence12)));
    }
};
std::shared_ptr<BridgeTransport> AcquireTransport(ID3D11DeviceContext* input) {
    if(!input||input->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)throw E_INVALIDARG;
    ComPtr<IUnknown> identity;ThrowIfFailed(input->QueryInterface(IID_PPV_ARGS(&identity)));
    struct Registry {std::mutex mutex;std::map<IUnknown*,std::weak_ptr<BridgeTransport>> contexts;};
    // Keep only weak entries; no global ownership of native contexts or devices.
    // The small registry itself survives DLL/process teardown.
    static auto* registry=new Registry;
    std::lock_guard lock(registry->mutex);
    for(auto it=registry->contexts.begin();it!=registry->contexts.end();)if(it->second.expired())it=registry->contexts.erase(it);else ++it;
    auto& weak=registry->contexts[identity.Get()];if(auto existing=weak.lock())return existing;
    auto transport=std::make_shared<BridgeTransport>(input);weak=transport;return transport;
}
}
struct Bridge11::Impl {
    std::shared_ptr<BridgeTransport> transport;
    ComPtr<ID3D11Device5>& device11;
    ComPtr<ID3D11DeviceContext4>& context;
    ComPtr<ID3D12Device>& device12;
    ComPtr<ID3D12CommandQueue>& queue;
    ComPtr<ID3D11Fence>& fence11;
    ComPtr<ID3D12Fence>& fence12;
    ComPtr<ID3D11ComputeShader> depthShader, motionShader;
    struct Slot {
        std::array<Twin,3> twins;
        Twin exposure;
        Twin output;
        Twin preview;
        ComPtr<ID3D12Resource> previewResult;
        Frame12 frame;
        std::shared_ptr<void> keepAlive;
        ComPtr<ID3D12Resource> result;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        ComPtr<ID3D12QueryHeap> stamps;     // Level 2 only: one timestamp pair per submission
        ComPtr<ID3D12Resource> stampData;
        UINT64 completion{};
    };
    struct Retired { std::shared_ptr<Slot> slot; UINT64 completion; };
    std::array<std::shared_ptr<Slot>,2> slots;
    std::vector<Retired> retired;
    ComPtr<ID3D11ShaderResourceView> image;
    ComPtr<ID3D11ShaderResourceView> previewImage;
    ComPtr<ID3D12Resource> queuedPreview;
    D3D12_RESOURCE_STATES previewState{};
    UINT64 serial{};
    UINT64 timestampFrequency{};
    std::atomic<float> chainMs{0}; // Level 2: last completed bridge recording, GPU milliseconds
    UINT64& value;
    bool& failed;
    const wchar_t* operation=L"Idle";
    explicit Impl(ID3D11DeviceContext* input):transport(AcquireTransport(input)),
        device11(transport->device11),context(transport->context),device12(transport->device12),
        queue(transport->queue),fence11(transport->fence11),fence12(transport->fence12),
        value(transport->value),failed(transport->failed) {
        for(auto& slot:slots) slot=NewSlot();
    }
    std::shared_ptr<Slot> NewSlot() {
        auto slot=std::make_shared<Slot>();
        ThrowIfFailed(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&slot->allocator)));
        ThrowIfFailed(device12->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,slot->allocator.Get(),nullptr,IID_PPV_ARGS(&slot->list)));
        ThrowIfFailed(slot->list->Close());
        // Timing is verbose-only work: no query heap exists at other levels.
        if(DetailLevel()>=Detail::Verbose) {
            if(!timestampFrequency) ThrowIfFailed(queue->GetTimestampFrequency(&timestampFrequency));
            D3D12_QUERY_HEAP_DESC heap{}; heap.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP; heap.Count=2;
            ThrowIfFailed(device12->CreateQueryHeap(&heap,IID_PPV_ARGS(&slot->stamps)));
            D3D12_HEAP_PROPERTIES props{}; props.Type=D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC buffer{}; buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width=2*sizeof(UINT64); buffer.Height=1; buffer.DepthOrArraySize=1; buffer.MipLevels=1;
            buffer.SampleDesc.Count=1; buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ThrowIfFailed(device12->CreateCommittedResource(&props,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&slot->stampData)));
        }
        return slot;
    }
    // Valid only after this slot's submission signal completed.
    void ReadStamps(Slot& slot) {
        if(!slot.stamps||!slot.stampData||!timestampFrequency) return;
        D3D12_RANGE range{0,2*sizeof(UINT64)}; void* data{};
        if(FAILED(slot.stampData->Map(0,&range,&data))||!data) return;
        const auto stamps=static_cast<const UINT64*>(data);
        if(stamps[1]>stamps[0])
            chainMs=float(double(stamps[1]-stamps[0])*1000.0/double(timestampFrequency));
        const D3D12_RANGE written{0,0}; slot.stampData->Unmap(0,&written);
    }
    HRESULT Share(Twin& t,D3D11_TEXTURE2D_DESC desc,bool converted=false) {
        desc.Usage=D3D11_USAGE_DEFAULT; desc.CPUAccessFlags=0;
        desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|(converted?D3D11_BIND_UNORDERED_ACCESS:0);
        desc.Format=StorageFormat(desc.Format);
        HRESULT hr=E_NOTIMPL;
        // Lite fallback: legacy handles are borrowed, only NT handles are closed.
        for(unsigned flavor=0;flavor<2;++flavor) {
            desc.MiscFlags=D3D11_RESOURCE_MISC_SHARED|(flavor==0?D3D11_RESOURCE_MISC_SHARED_NTHANDLE:0);
            t.texture.Reset(); t.resource.Reset(); t.handle=std::make_shared<Handle>();
            hr=device11->CreateTexture2D(&desc,nullptr,&t.texture); if(FAILED(hr)) continue;
            if(flavor==0) {
                ComPtr<IDXGIResource1> r; hr=t.texture.As(&r);
                if(SUCCEEDED(hr)) hr=r->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&t.handle->value);
                t.handle->owned=true;
            } else {
                ComPtr<IDXGIResource> r; hr=t.texture.As(&r);
                if(SUCCEEDED(hr)) hr=r->GetSharedHandle(&t.handle->value);
            }
            if(FAILED(hr)) continue;
            hr=device12->OpenSharedHandle(t.handle->value,IID_PPV_ARGS(&t.resource));
            if(SUCCEEDED(hr)) return S_OK;
        }
        return hr;
    }
    void MakeTwin(Twin& t,ID3D11Resource* input,unsigned index,unsigned frameWidth=0,unsigned frameHeight=0) {
        if(!input) return;
        ComPtr<ID3D11Device> owner; input->GetDevice(&owner);
        if(owner.Get()!=static_cast<ID3D11Device*>(device11.Get())) throw E_INVALIDARG;
        ComPtr<ID3D11Texture2D> texture; ThrowIfFailed(input->QueryInterface(IID_PPV_ARGS(&texture)));
        texture->GetDesc(&t.desc); if(!Plain(t.desc)) throw E_NOTIMPL;
        t.source=input;
        const bool depth=index==1;
        if(depth) {
            // Convert straight into a frame-sized R32_FLOAT twin: one dispatch does the format
            // change and the downsample, so the model gets depth at its own resolution.
            auto shared=t.desc;
            shared.Format=DXGI_FORMAT_R32_FLOAT;
            if(frameWidth && frameWidth<shared.Width) shared.Width=frameWidth;
            if(frameHeight && frameHeight<shared.Height) shared.Height=frameHeight;
            ThrowIfFailed(Share(t,shared,true)); t.converted=true;
            t.outWidth=shared.Width; t.outHeight=shared.Height;
            ThrowIfFailed(device11->CreateUnorderedAccessView(t.texture.Get(),nullptr,&t.uav));
            D3D11_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format=ViewFormat(t.desc.Format,true);
            srv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D; srv.Texture2D.MipLevels=1;
            // A typeless source can be sampled directly; otherwise it needs a typeless staging copy.
            const auto direct=device11->CreateShaderResourceView(texture.Get(),&srv,&t.view);
            if(FAILED(direct)) {
                auto staging=t.desc; staging.Format=StorageFormat(staging.Format); staging.Usage=D3D11_USAGE_DEFAULT;
                staging.BindFlags=D3D11_BIND_SHADER_RESOURCE; staging.CPUAccessFlags=0; staging.MiscFlags=0;
                ThrowIfFailed(device11->CreateTexture2D(&staging,nullptr,&t.backup));
                ThrowIfFailed(device11->CreateShaderResourceView(t.backup.Get(),&srv,&t.view));
            }
            lite::LogVerbose(SUCCEEDED(direct)?L"depth bridge: direct SRV (no staging copy)"
                                             :L"depth bridge: staging copy (source is not typeless)");
            if(!depthShader) ThrowIfFailed(device11->CreateComputeShader(bridgeDepthShader,sizeof(bridgeDepthShader),nullptr,&depthShader));
            return;
        }
        auto shared=t.desc;
        const bool wideMotion=index==2 && ViewFormat(shared.Format)==DXGI_FORMAT_R32G32_FLOAT;
        HRESULT hr=wideMotion?E_NOTIMPL:Share(t,shared);
        if(FAILED(hr) && index==2) {
            const auto format=ViewFormat(t.desc.Format,false);
            if(format!=DXGI_FORMAT_R32G32_FLOAT && format!=DXGI_FORMAT_R16G16_FLOAT) throw hr;
            shared.Format=DXGI_FORMAT_R16G16_FLOAT;
            ThrowIfFailed(Share(t,shared,true)); t.converted=true;
            t.outWidth=shared.Width; t.outHeight=shared.Height;
            auto backup=t.desc; backup.Format=StorageFormat(backup.Format); backup.Usage=D3D11_USAGE_DEFAULT;
            backup.BindFlags=D3D11_BIND_SHADER_RESOURCE; backup.CPUAccessFlags=0; backup.MiscFlags=0;
            ThrowIfFailed(device11->CreateTexture2D(&backup,nullptr,&t.backup));
            D3D11_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format=format;
            srv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D; srv.Texture2D.MipLevels=1;
            ThrowIfFailed(device11->CreateShaderResourceView(t.backup.Get(),&srv,&t.view));
            ThrowIfFailed(device11->CreateUnorderedAccessView(t.texture.Get(),nullptr,&t.uav));
            if(!motionShader) ThrowIfFailed(device11->CreateComputeShader(bridgeMotionShader,sizeof(bridgeMotionShader),nullptr,&motionShader));
        } else ThrowIfFailed(hr);
    }
    bool Matches(Slot& s,const Input11& input,unsigned width,unsigned height,DXGI_FORMAT format) {
        if(!s.output.texture || s.output.desc.Width!=width || s.output.desc.Height!=height || s.output.desc.Format!=format) return false;
        auto same=[&](Twin& twin,ID3D11Resource* source){
            // Same object: its device and descriptor are already the ones this twin was built for.
            if(twin.source.Get()==source)return true;
            if(bool(twin.source)!=bool(source))return false;if(!source)return true;
            ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11Device> owner;source->GetDevice(&owner);
            if(owner.Get()!=static_cast<ID3D11Device*>(device11.Get())||FAILED(source->QueryInterface(IID_PPV_ARGS(&texture))))return false;
            D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);const auto& old=twin.desc;
            return d.Width==old.Width&&d.Height==old.Height&&d.Format==old.Format&&d.ArraySize==old.ArraySize&&d.MipLevels==old.MipLevels&&d.SampleDesc.Count==old.SampleDesc.Count&&d.SampleDesc.Quality==old.SampleDesc.Quality;
        };
        for(unsigned n=0;n<3;++n)if(!same(s.twins[n],input.resources[n]))return false;
        if(!same(s.exposure,input.exposureResource))return false;
        // Slot fence has completed; only the source owner changes, not the shared twin.
        for(unsigned n=0;n<3;++n)s.twins[n].source=input.resources[n];s.exposure.source=input.exposureResource;
        return true;
    }
    void Fill(Twin& t,unsigned index) {
        if(!t.source) return;
        if(!t.converted) { context->CopyResource(t.texture.Get(),t.source.Get()); return; }
        if(t.backup) context->CopyResource(t.backup.Get(),t.source.Get());
        ComputeState saved(context.Get());
        auto* srv=t.view.Get(); auto* uav=t.uav.Get(); UINT keep=UINT(-1);
        context->CSSetShaderResources(0,1,&srv); context->CSSetUnorderedAccessViews(0,1,&uav,&keep);
        context->CSSetShader(index==1?depthShader.Get():motionShader.Get(),nullptr,0);
        const auto w=t.outWidth?t.outWidth:t.desc.Width, h=t.outHeight?t.outHeight:t.desc.Height;
        context->Dispatch((w+7)/8,(h+7)/8,1);
    }
    HRESULT Process(const Input11& input,unsigned width,unsigned height,DXGI_FORMAT format,const Bridge11::Recorder& record) {
        std::lock_guard lock(transport->mutex);
        operation=L"Validate input / completion";
        if(failed) return DXGI_ERROR_DEVICE_REMOVED;
        if(!record || !width || !height || format==DXGI_FORMAT_UNKNOWN) return E_INVALIDARG;
        auto& slot=slots[serial%2];
        const auto completed=fence12->GetCompletedValue();
        if(completed==UINT64_MAX) { failed=true; return device12->GetDeviceRemovedReason(); }
        // Authorized divergence: no work, no resource replacement, no CPU wait if busy.
        if(slot->completion>completed) return S_FALSE;
        // This slot's previous submission is finished, so its timestamps are readable.
        ReadStamps(*slot);
        for(auto it=retired.begin();it!=retired.end();) {
            if(it->completion<=completed) { ReadStamps(*it->slot); it=retired.erase(it); }
            else ++it;
        }
        if(!Matches(*slot,input,width,height,format)) {
            operation=L"Allocate shared textures";
            {
                wchar_t text[192]{};
                swprintf_s(text,L"bridge: slot rebuilt for %ux%u fmt=%u (shared textures, fence signal, flush)",width,height,unsigned(format));
                lite::LogVerbose(text);
            }
            auto fresh=NewSlot();
            // A game-declared depth subrect (guide extents/origins) is expressed in source pixels and
            // reaches the model as its Depth region, so such a twin keeps the source size. Only
            // plugin-owned depth, which has no declared region, is scaled down to the frame.
            const bool scaleDepth=!input.guides.width[1]&&!input.guides.height[1]&&
                                  !input.guides.originX[1]&&!input.guides.originY[1];
            for(unsigned n=0;n<3;++n) { MakeTwin(fresh->twins[n],input.resources[n],n,scaleDepth?width:0,scaleDepth?height:0); fresh->frame.resources[n]=fresh->twins[n].resource; }
            if(input.exposureResource){MakeTwin(fresh->exposure,input.exposureResource,0);auto d=fresh->exposure.desc;if(d.Width!=1||d.Height!=1)throw E_NOTIMPL;fresh->frame.exposureTexture=fresh->exposure.resource;}
            D3D11_TEXTURE2D_DESC desc{}; desc.Width=width; desc.Height=height; desc.ArraySize=desc.MipLevels=1;
            desc.SampleDesc.Count=1; desc.Format=format;
            // An unchanged storage format can carry a UAV, so the model resolves into the shared
            // output directly and this bridge skips one full-frame result copy.
            const bool directOutput=StorageFormat(format)==format&&ViewFormat(format)==format;
            ThrowIfFailed(Share(fresh->output,desc,directOutput)); fresh->output.desc=desc;
            fresh->frame.colorOutput=directOutput?fresh->output.resource.Get():nullptr;
            {
                wchar_t text[160]{};
                swprintf_s(text,L"bridge: result %ls (fmt=%u %ux%u)",directOutput?L"resolved in place":L"copied back",
                           unsigned(format),width,height);
                lite::LogVerbose(text);
            }
            // This marker covers any prior ImGui reads on the native context.
            const auto retirement=++value; ThrowIfFailed(context->Signal(fence11.Get(),retirement)); context->Flush();
            retired.push_back({slot,retirement}); slot=std::move(fresh);
        }
        slot->frame.guides=input.guides;
        slot->frame.origin=input.origin;
        slot->frame.generatedMotion=false;slot->frame.frameTimeMs=input.frameTimeMs;
        for(unsigned n=0;n<3;++n)slot->frame.converted[n]=slot->twins[n].converted;
        operation=L"Reset command allocator / list";
        // Validate and allocate descriptors before recording copies on the game context.
        ThrowIfFailed(slot->allocator->Reset()); ThrowIfFailed(slot->list->Reset(slot->allocator.Get(),nullptr));
        Recording recording{slot->list.Get()};
        slot->frame.ticket=std::make_shared<GpuTicket>();
        slot->frame.orderedQueue=queue;
        struct TicketGuard {std::shared_ptr<GpuTicket> ticket;~TicketGuard(){ticket->Discard();}} ticketGuard{slot->frame.ticket};
        for(auto& r:slot->frame.resources) Transition(slot->list.Get(),r.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(slot->list.Get(),slot->frame.exposureTexture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if(slot->stamps) slot->list->EndQuery(slot->stamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
        ID3D12Resource* result{}; D3D12_RESOURCE_STATES resultState{};
        operation=L"Record flow / model";
        queuedPreview.Reset();
        auto hr=record(slot->list.Get(),slot->frame,slot->keepAlive,result,resultState);
        if(failed){recording.Close();return DXGI_ERROR_DEVICE_REMOVED;}
        if(hr!=S_OK) { recording.Close(); return hr; }
        if(!result) { recording.Close(); return E_POINTER; }
        operation=L"Validate bridge result dimensions / format";
        const auto resultDesc=result->GetDesc();
        ComPtr<ID3D12Device> resultDevice; ThrowIfFailed(result->GetDevice(IID_PPV_ARGS(&resultDevice)));
        if(resultDevice.Get()!=device12.Get() || resultDesc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
           resultDesc.DepthOrArraySize!=1 || resultDesc.MipLevels!=1 || resultDesc.SampleDesc.Count!=1 ||
           resultDesc.Width!=width || resultDesc.Height!=height || !CopyCompatible(resultDesc.Format,format)) {
            recording.Close(); return E_INVALIDARG;
        }
        slot->result=result;
        slot->previewResult.Reset();
        if(queuedPreview)try{
            auto d=queuedPreview->GetDesc();ComPtr<ID3D12Device> owner;ThrowIfFailed(queuedPreview->GetDevice(IID_PPV_ARGS(&owner)));
            if(owner.Get()!=device12.Get()||d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||d.DepthOrArraySize!=1||d.MipLevels!=1||d.SampleDesc.Count!=1)throw E_INVALIDARG;
            if(!slot->preview.texture||slot->preview.desc.Width!=d.Width||slot->preview.desc.Height!=d.Height||slot->preview.desc.Format!=d.Format){
                Twin fresh;D3D11_TEXTURE2D_DESC desc{};desc.Width=unsigned(d.Width);desc.Height=d.Height;desc.ArraySize=desc.MipLevels=desc.SampleDesc.Count=1;desc.Format=d.Format;
                ThrowIfFailed(Share(fresh,desc));fresh.desc=desc;
                if(slot->preview.texture){auto old=std::make_shared<Slot>();old->preview=std::move(slot->preview);const auto retirement=++value;ThrowIfFailed(context->Signal(fence11.Get(),retirement));context->Flush();retired.push_back({old,retirement});}
                slot->preview=std::move(fresh);
            }
            slot->previewResult=queuedPreview;
        }catch(HRESULT previewError){if(previewError!=E_OUTOFMEMORY)throw;slot->previewResult.Reset();previewImage.Reset();}
        operation=L"Copy DX11 inputs / signal ready";
        for(unsigned n=0;n<3;++n) Fill(slot->twins[n],n);
        Fill(slot->exposure,0);
        const auto ready=++value; ThrowIfFailed(context->Signal(fence11.Get(),ready)); context->Flush();
        ThrowIfFailed(queue->Wait(fence12.Get(),ready));
        if(result==slot->output.resource.Get()) {
            // The recorder resolved into the shared output in place: only a state hand-off remains.
            Transition(slot->list.Get(),result,resultState,D3D12_RESOURCE_STATE_COMMON);
        } else {
            Transition(slot->list.Get(),result,resultState,D3D12_RESOURCE_STATE_COPY_SOURCE);
            Transition(slot->list.Get(),slot->output.resource.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
            slot->list->CopyResource(slot->output.resource.Get(),result);
            Transition(slot->list.Get(),slot->output.resource.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
            Transition(slot->list.Get(),result,D3D12_RESOURCE_STATE_COPY_SOURCE,resultState);
        }
        if(slot->previewResult){Transition(slot->list.Get(),slot->previewResult.Get(),previewState,D3D12_RESOURCE_STATE_COPY_SOURCE);Transition(slot->list.Get(),slot->preview.resource.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);slot->list->CopyResource(slot->preview.resource.Get(),slot->previewResult.Get());Transition(slot->list.Get(),slot->preview.resource.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);Transition(slot->list.Get(),slot->previewResult.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,previewState);}
        for(auto& r:slot->frame.resources) Transition(slot->list.Get(),r.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        Transition(slot->list.Get(),slot->frame.exposureTexture.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        if(slot->stamps) {
            slot->list->EndQuery(slot->stamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);
            slot->list->ResolveQueryData(slot->stamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,slot->stampData.Get(),0);
        }
        operation=L"Close command list";ThrowIfFailed(recording.Close());
        // Never add a native GPU wait unless the submission signal succeeded.
        ID3D12CommandList* lists[]{slot->list.Get()};
        // Once executed, unknown completion poisons the shared timeline. Peers
        // must not advance it and accidentally retire this submission's owners.
        struct SubmissionGuard {bool& failed;bool active=true;~SubmissionGuard(){if(active)failed=true;}} submission{failed};
        queue->ExecuteCommandLists(1,lists); slot->completion=UINT64_MAX;
        operation=L"Signal submitted DX12 work";
        const auto done=++value; hr=queue->Signal(fence12.Get(),done);
        slot->frame.ticket->Submit(SUCCEEDED(hr)?fence12.Get():nullptr,SUCCEEDED(hr)?done:0,queue.Get());
        if(FAILED(hr)) { failed=true; return hr; }
        operation=L"DX11 GPU wait / result view";ThrowIfFailed(context->Wait(fence11.Get(),done));
        // The completion signal rides the game's own batch: it is reached when the game submits,
        // which always happens before the next Process call. Flushing here would only split the
        // game's frame into one more submission.
        const auto completion=++value;ThrowIfFailed(context->Signal(fence11.Get(),completion));
        slot->completion=completion;submission.active=false;
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format=ViewFormat(format);
        srv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D; srv.Texture2D.MipLevels=1;
        if(!slot->output.view)ThrowIfFailed(device11->CreateShaderResourceView(slot->output.texture.Get(),&srv,&slot->output.view));
        image=slot->output.view;
        if(slot->previewResult){srv.Format=ViewFormat(slot->preview.desc.Format);if(!slot->preview.view)ThrowIfFailed(device11->CreateShaderResourceView(slot->preview.texture.Get(),&srv,&slot->preview.view));previewImage=slot->preview.view;}else previewImage.Reset();
        ++serial;
        return S_OK;
    }
    HRESULT Drain() {
        std::lock_guard lock(transport->mutex);
        // Teardown is distinct from the nonblocking per-frame path.
        if(failed) return DXGI_ERROR_DEVICE_REMOVED;
        for(const auto& slot:slots) if(slot->completion==UINT64_MAX) return E_FAIL;
        const auto done=++value; auto hr=context->Signal(fence11.Get(),done);
        if(FAILED(hr)) return hr; context->Flush();
        if(fence12->GetCompletedValue()>=done) return S_OK;
        Handle event; event.owned=true; event.value=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if(!event.value) return HRESULT_FROM_WIN32(GetLastError());
        hr=fence12->SetEventOnCompletion(done,event.value); if(FAILED(hr)) return hr;
        return WaitForSingleObject(event.value,5000)==WAIT_OBJECT_0?S_OK:HRESULT_FROM_WIN32(WAIT_TIMEOUT);
    }
};
Bridge11::Bridge11(ID3D11DeviceContext* c):impl(std::make_unique<Impl>(c)) {}
Bridge11::~Bridge11() {
    // A timeout is not completion. Quarantine GPU-owned state until process exit.
    if(impl && FAILED(impl->Drain())) impl.release();
}
HRESULT Bridge11::Process(const Input11& in,unsigned w,unsigned h,DXGI_FORMAT format,const Recorder& record) {
    try {
        auto hr=impl->Process(in,w,h,format,record);
        return hr;
    }
    catch(HRESULT hr) { return hr; } catch(...) { return E_FAIL; }
}
ComPtr<ID3D11ShaderResourceView> Bridge11::Image() const { return impl->image; }
void Bridge11::QueuePreview(ID3D12Resource* resource,D3D12_RESOURCE_STATES state){impl->queuedPreview=resource;impl->previewState=state;}
ComPtr<ID3D11ShaderResourceView> Bridge11::PreviewImage() const{return impl->previewImage;}
ID3D12Device* Bridge11::Device() const { return impl->device12.Get(); }
const wchar_t* Bridge11::LastOperation() const { return impl->operation; }
float Bridge11::ChainMilliseconds() const { return impl->chainMs.load(); }
}

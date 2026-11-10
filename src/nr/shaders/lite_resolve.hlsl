// SPDX-License-Identifier: GPL-3.0-or-later
// The NR answer in the game's own signal: proxy decode, white-point restore, the
// OkLab compose that keeps the original highlights, gain and alpha.
// Former lite_color mode 1.
cbuffer Params:register(b0) {
 uint w,h,display;float white,preMul;uint liveExposure;
 float differenceGain;uint composed;float exponent,trim;uint reserved;
};
Texture2D<float4> source:register(t0),model:register(t1),original:register(t2),exposure:register(t3);
RWTexture2D<float4> target:register(u0);
#include "lite_common.hlsli"
[numthreads(8,8,1)]void Main(uint3 id:SV_DispatchThreadID){
 if(id.x>=w||id.y>=h)return;
 float4 base=original.Load(int3(id.xy,0));
 base.rgb=max(base.rgb,0);
 float3 proxy=source.Load(int3(id.xy,0)).rgb;
 float3 modelRaw=model.Load(int3(id.xy,0)).rgb;float3 raw=modelRaw;
 if(!display){proxy=SrgbToLinear(proxy);raw=SrgbToLinear(raw);}

 float3 answer=display?raw:Decode(raw);
 float norm=display?1:White();float originalLuma=dot(base.rgb,float3(.2126,.7152,.0722))/norm;
 float modelLuma=dot(answer,float3(.2126,.7152,.0722));float3 result=base.rgb/norm;
 if(composed)result=Compose(result,proxy,raw);
 else if(modelLuma>1e-5)result=answer*(clamp(modelLuma,originalLuma/MaxLuminanceRatio,originalLuma*MaxLuminanceRatio)/modelLuma);
 result*=norm;
 result=ApplyGain(base.rgb,result);
 target[id.xy]=float4(max(result,0),base.a);
}

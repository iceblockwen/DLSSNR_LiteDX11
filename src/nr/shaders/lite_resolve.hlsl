// SPDX-License-Identifier: GPL-3.0-or-later
// The NR answer in the game's own signal: proxy decode, white-point restore, the
// OkLab compose that keeps the original highlights, gain, unsharp and alpha.
// Former lite_color mode 1.
cbuffer Params:register(b0) {
 uint w,h,display;float white,preMul;uint liveExposure;
 float differenceGain;uint composed;float exponent,trim;uint reserved;
 float sharpness;
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
 // Unsharp mask from the model answer, in the same proxy domain as the resolve and
 // after Compose, whose last step rewrites the local luma. The neighbourhood is read
 // from `model`, never from `original`: with a bridge-provided colour output that
 // texture is the one being written. Recorded before ApplyGain so DifferenceGain
 // scales the sharpening with the rest of the difference. A frame-uniform branch:
 // sharpness 0 costs nothing.
 if(sharpness>0){
  uint2 p=max(id.xy-1u,0u),q=min(id.xy+1u,uint2(w-1,h-1));
  float3 n0=model.Load(int3(p.x,id.y,0)).rgb,n1=model.Load(int3(q.x,id.y,0)).rgb;
  float3 n2=model.Load(int3(id.x,p.y,0)).rgb,n3=model.Load(int3(id.x,q.y,0)).rgb;
  float3 blur=(n0+n1+n2+n3)*.25;
  float3 lo=min(min(n0,n1),min(n2,n3)),hi=max(max(n0,n1),max(n2,n3));
  result+=clamp(modelRaw-blur,lo-blur,hi-blur)*norm*sharpness;
 }
 result=ApplyGain(base.rgb,result);
 target[id.xy]=float4(max(result,0),base.a);
}

// SPDX-License-Identifier: GPL-3.0-or-later
// The model's colour input: reversible HDR proxy, identity below the knee and
// bounded highlights above it. Former lite_color mode 0.
cbuffer Params:register(b0) {
 uint w,h,display;float white,preMul;uint liveExposure;
 float differenceGain;uint composed;float exponent,trim;uint reserved;
};
Texture2D<float4> source:register(t0),exposure:register(t3);
RWTexture2D<float4> target:register(u0);
#include "lite_common.hlsli"
[numthreads(8,8,1)]void Main(uint3 id:SV_DispatchThreadID){
 if(id.x>=w||id.y>=h)return;
 float4 s=source.Load(int3(id.xy,0));float3 c=max(s.rgb,0);
 target[id.xy]=display?float4(c,s.a):float4(LinearToSrgb(Encode(c/White())),1);
}

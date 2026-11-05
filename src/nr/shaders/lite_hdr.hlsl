// SPDX-License-Identifier: GPL-3.0-or-later
// RenoDX-derived inverse tone mapping; see Licenses/RenoDX_HDR_MIT.txt.
#include "hdr_math.h"
cbuffer Params : register(b0) {
 uint width,height,method,encoding;float peak,sdrWhite,grayInput,grayNits;
};
Texture2D<float4> inputImage : register(t0);
RWTexture2D<float4> outputImage : register(u0);
float3 Pq(float3 nits){
 float3 p=pow(saturate(nits/10000.f),2610.f/16384.f);
 return pow((3424.f/4096.f+(2413.f/128.f)*p)/(1.f+(2392.f/128.f)*p),2523.f/32.f);
}
float3 To2020(float3 rgb){
 return mul(float3x3(.627404,.329283,.043313,.069097,.919540,.011363,.016391,.088013,.895596),rgb);
}
float3 Bt2446(float3 bt2020){
 // Matches RenoDX inverse::bt2446a::BT2020 color reconstruction.
 float3 encoded=pow(max(bt2020,0.f),1.f/2.4f);
 const float3 k=float3(.262698338956556,.678008765772817,.0592928952706273);
 float y=dot(encoded,k);
 float3 chromas=2.f-2.f*k;
 float3 chroma=(encoded-y)/chromas;
 float ys=y+max(.1f*chroma.r,0.f);
 float yh=Bt2446Luma(ys,sdrWhite,peak);
 float scale=yh>0.f?ys/(1.1f*yh):1.f;
 float3 result=scale>1e-7f?(chromas*chroma)/scale+yh:float3(yh,yh,yh);
 return pow(saturate(result),2.4f)*peak;
}
[numthreads(8,8,1)]void Main(uint3 id:SV_DispatchThreadID){
 if(id.x>=width||id.y>=height)return;
 float4 src=inputImage.Load(int3(id.xy,0));
 float3 rgb=float3(DecodeSdr(src.r,encoding),DecodeSdr(src.g,encoding),DecodeSdr(src.b,encoding));
 float3 nits;
 if(method==0){
  float y=dot(rgb,float3(.2126,.7152,.0722));
  float mapped=InverseReinhard(y,grayInput,grayNits,peak);
  nits=To2020(rgb)*(y>1e-7f?mapped/y:0.f);
  // Uniform scale to fit the destination gamut; avoid independent channel clipping.
  float highest=max(nits.r,max(nits.g,nits.b));
  if(highest>peak)nits*=peak/highest;
 }else nits=Bt2446(To2020(rgb));
 outputImage[id.xy]=float4(Pq(nits),src.a);
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Typed re-write of a guide texture: depth and motion vectors get a model-typed
// twin without moving or changing a value. Former lite_color mode 2.
cbuffer Params:register(b0) { uint w,h; };
Texture2D<float4> source:register(t0);
RWTexture2D<float4> target:register(u0);
[numthreads(8,8,1)]void Main(uint3 id:SV_DispatchThreadID){
 if(id.x>=w||id.y>=h)return;
 target[id.xy]=source.Load(int3(id.xy,0));
}

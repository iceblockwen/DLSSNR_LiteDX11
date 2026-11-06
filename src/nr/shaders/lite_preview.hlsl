
Texture2D<float4> source : register(t0);
RWTexture2D<float4> target : register(u0);
cbuffer Settings : register(b0) {
 uint mode, ox, oy, sw;
 uint sh, ow, oh, flags;
 float sx, sy, gain;
 uint tileX;
};
[numthreads(8,8,1)] void Main(uint3 id:SV_DispatchThreadID) {
 if(id.x>=ow || id.y>=oh) return;
 uint2 at = uint2((float2(id.xy)+0.5)*float2(sw,sh)/float2(ow,oh));
 at=min(at,uint2(sw-1,sh-1));
 if(mode==2 && (flags&2)) at.y=sh-1-at.y;
 float4 v=source.Load(int3(at+uint2(ox,oy),0));
 float4 result;
 if(mode==2) {
  float d=(flags&1)?1-v.x:v.x;
  if(flags&4)d=pow(saturate(1-d),0.125);
  result=float4(saturate(d*gain).xxx,1);
 } else if(mode==3) {
  float2 mv=v.xy*float2(sx,sy)*gain;
  result=float4(saturate(mv*0.5+0.5),0.5,1);
 } else result=float4(saturate(v.rgb*gain),1);
 if(!all(isfinite(v))) result=float4(1,0,1,1);
 target[uint2(id.x+tileX, id.y)]=result;
}
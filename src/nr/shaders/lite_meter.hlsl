
Texture2D<float4> source:register(t0);Texture2D<float4> gameExposure:register(t1);RWTexture2D<float> grid:register(u0);
[numthreads(8,8,1)]void Main(uint3 id:SV_DispatchThreadID){
 if(any(id.xy>=64))return;uint w,h;source.GetDimensions(w,h);
 if(!any(id.xy)){grid[id.xy]=gameExposure.Load(int3(0,0,0)).r;return;}
 uint2 begin=id.xy*uint2(w,h)/64,end=(id.xy+1)*uint2(w,h)/64;
 uint2 step=max((end-begin)/8,1);float sum=0;uint taken=0;
 for(uint y=begin.y;y<max(end.y,begin.y+1);y+=step.y)
 for(uint x=begin.x;x<max(end.x,begin.x+1);x+=step.x){
  float3 c=max(source.Load(int3(min(uint2(x,y),uint2(w-1,h-1)),0)).rgb,0);
  float l=dot(c,float3(0.2126,0.7152,0.0722));if(isfinite(l)){sum+=l;taken++;}
 }
 grid[id.xy]=taken?sum/taken:asfloat(0x7fc00000);
}
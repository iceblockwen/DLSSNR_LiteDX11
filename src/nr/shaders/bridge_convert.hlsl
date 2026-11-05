
Texture2D<float> depthIn:register(t0);
RWTexture2D<float> depthOut:register(u0);
// Output may be smaller than the source: sample the centre of each output pixel's footprint.
// A depth buffer is never filtered, so this stays a point sample.
[numthreads(8,8,1)] void Depth(uint3 id:SV_DispatchThreadID){
 uint ow,oh;depthOut.GetDimensions(ow,oh);if(id.x>=ow||id.y>=oh)return;
 uint iw,ih;depthIn.GetDimensions(iw,ih);
 const uint sx=min(iw-1u,id.x*iw/ow), sy=min(ih-1u,id.y*ih/oh);
 depthOut[id.xy]=depthIn.Load(int3(sx,sy,0));
}
Texture2D<float2> motionIn:register(t0);
RWTexture2D<float2> motionOut:register(u0);
[numthreads(8,8,1)] void Motion(uint3 id:SV_DispatchThreadID){
 uint w,h;motionOut.GetDimensions(w,h);if(id.x<w&&id.y<h)motionOut[id.xy]=motionIn.Load(int3(id.xy,0));
}
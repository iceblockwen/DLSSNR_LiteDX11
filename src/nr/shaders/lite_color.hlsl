// SPDX-License-Identifier: GPL-3.0-or-later
// Reversible HDR proxy: identity below the knee, bounded highlights above it.
// Encode/resolve keep the original white-point contract.
cbuffer Params:register(b0) {
 uint mode,w,h,display;float white,preMul;uint liveExposure;uint guideX,guideY,guideW,guideH;
 float differenceGain;uint composed;float exponent,trim;uint reserved;
};
Texture2D<float4> source:register(t0),model:register(t1),original:register(t2),exposure:register(t3);
RWTexture2D<float4> target:register(u0);
// Fixed safety bound: no UI/INI switch, matches the former MaxRatio=16 default.
static const float MaxLuminanceRatio=16.0;
float White(){
 float v=clamp(white,1e-8,1024);
 if(liveExposure){
  float e=exposure.Load(int3(0,0,0)).r;
  if(e>1e-6&&e<1e6){
   // Game exposure ratio; exponent 1 is the plain linear mapping, below 1 compresses
   // the exposure range so a single trim covers both bright and dark scenes.
   // liveExposure 1 is pre-exposure/exposure, 2 swaps the fraction. The texture
   // value is never inverted, only its position in the fraction changes.
   float ratio=max(liveExposure==2?e/max(preMul,1e-8):preMul/max(e,1e-8),1e-8);
   v=clamp((exponent==1.0?ratio:pow(ratio,exponent))*trim,1e-8,1024);
  }
 }
 return v;
}
float3 LinearToSrgb(float3 v){v=saturate(v);return lerp(v*12.92,1.055*pow(max(v,1e-8),1.0/2.4)-.055,step(.0031308,v));}
float3 SrgbToLinear(float3 v){v=saturate(v);return lerp(v/12.92,pow((v+.055)/1.055,2.4),step(.04045,v));}
float Curve(float m){const float k=.75;if(m<=k)return m;float e=(m-k)/(1-k);return k+(1-k)*e*rsqrt(e*e+1);}
float Inverse(float y){const float k=.75;if(y<=k)return y;float u=min((y-k)/(1-k),.999999);return k+(1-k)*u*rsqrt(max(1-u*u,1e-8));}
float3 Encode(float3 v){v=max(v,0);float m=max(v.r,max(v.g,v.b));return m<=1e-6?v:v*(Curve(m)/m);}
float3 Decode(float3 v){v=max(v,0);float m=max(v.r,max(v.g,v.b));return m<=1e-6?v:v*(Inverse(m)/m);}
// Composition follows RenoDX/ShortFuse via Skynizz's attributed implementation.
// See Licenses/RenoDX_ATTRIBUTION.txt. OkLab matrices: Bjorn Ottosson.
float Luma(float3 v){return dot(v,float3(.2126,.7152,.0722));}
float3 ToOkLab(float3 v){
 float3 lms=mul(float3x3(.4122214708,.5363325363,.0514459929,.2119034982,.6806995451,.1073969566,.0883024619,.2817188376,.6299787005),v);
 lms=sign(lms)*pow(abs(lms),1.0/3.0);
 return mul(float3x3(.2104542553,.7936177850,-.0040720468,1.9779984951,-2.4285922050,.4505937099,.0259040371,.7827717662,-.8086757660),lms);
}
float3 FromOkLab(float3 v){
 float3 lms=mul(float3x3(1,.3963377774,.2158037573,1,-.1055613458,-.0638541728,1,-.0894841775,-1.2914855480),v);
 return mul(float3x3(4.0767416621,-3.3077115913,.2309699292,-1.2684380046,2.6097574011,-.3413193965,-.0041960863,-.7034186147,1.7076147010),lms*lms*lms);
}
// Reduce chroma toward equal-luminance neutral rather than clip channels.
float3 NeutralGamut(float3 v){
 float y=Luma(v);if(y<=1e-8)return max(v,0);
 float scale=1;
 if(v.r<0)scale=min(scale,y/(y-v.r));
 if(v.g<0)scale=min(scale,y/(y-v.g));
 if(v.b<0)scale=min(scale,y/(y-v.b));
 return y+(v-y)*scale;
}
float3 CorrectHue(float3 scaled,float3 modelColour){
 float3 lab=ToOkLab(scaled),reference=ToOkLab(modelColour);
 float chroma=length(reference.yz);
 lab.yz=(chroma>1e-5?reference.yz/chroma:float2(0,0))*length(lab.yz);
 return NeutralGamut(FromOkLab(lab));
}
float3 Compose(float3 base,float3 proxy,float3 raw){
 float originalLuma=Luma(base),proxyLuma=Luma(proxy),modelLuma=Luma(raw);
 if(modelLuma<=1e-5)return base;
 float scale=originalLuma<proxyLuma?originalLuma/max(proxyLuma,1e-6):
     (modelLuma+max(0,originalLuma-proxyLuma))/modelLuma;
 float3 upgraded=CorrectHue(raw*scale,raw);

 float editRatio=originalLuma>1e-8?Luma(upgraded)/originalLuma:1;
 float bounded=clamp(editRatio,1.0/MaxLuminanceRatio,MaxLuminanceRatio);
 float upgradedLuma=Luma(upgraded);
 upgraded=upgradedLuma>1e-8?upgraded*(originalLuma*bounded/upgradedLuma):base*bounded;
 return upgraded;
}
// Preserve negative residuals for darkening, clamp the amplified RGB result.
// Do not saturate to 1: scene-linear HDR values can legitimately exceed it.
float3 ApplyGain(float3 base,float3 result){return max(base+clamp(differenceGain,0,2)*(result-base),0);}
[numthreads(8,8,1)]void Main(uint3 id:SV_DispatchThreadID){
 if(id.x>=w||id.y>=h)return;
 if(mode==2){uint2 at=id.xy;
  if(display&&id.x>=guideX&&id.x<guideX+guideW&&id.y>=guideY&&id.y<guideY+guideH)
   at.y=guideY+guideH-1-(id.y-guideY);
  target[id.xy]=source.Load(int3(at,0));return;} // typed conversion; flip only the declared depth region
 if(mode==0){float4 s=source.Load(int3(id.xy,0));float3 c=max(s.rgb,0);
 target[id.xy]=display?float4(c,s.a):float4(LinearToSrgb(Encode(c/White())),1);return;}
 float4 base=original.Load(int3(id.xy,0));
 base.rgb=max(base.rgb,0);
 float3 proxy=source.Load(int3(id.xy,0)).rgb;
 float3 raw=model.Load(int3(id.xy,0)).rgb;
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

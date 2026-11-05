// Inverse tone-map equations adapted from RenoDX inverse_tonemap.hlsl.
// Copyright (c) 2025 Carlos Lopez Jr. MIT License; see Licenses/RenoDX_HDR_MIT.txt.
// Shared scalar code for the GPU and the neutral-ramp UI plot.
#ifndef LITE_HDR_MATH_H
#define LITE_HDR_MATH_H
#ifdef __cplusplus
#include <cmath>
#include <algorithm>
namespace lite::hdr::math {
inline float Hpow(float a,float b){return std::pow(a,b);}
inline float Hlog(float a){return std::log(a);}
inline float Hsqrt(float a){return std::sqrt(a);}
inline float Hmin(float a,float b){return (std::min)(a,b);}
inline float Hmax(float a,float b){return (std::max)(a,b);}
#else
#define Hpow pow
#define Hlog log
#define Hsqrt sqrt
#define Hmin min
#define Hmax max
#endif
// Encoding 0=sRGB, 1=Gamma2.2, 2=Gamma2.4 (zero-black BT.1886 approximation).
inline float DecodeSdr(float x,unsigned int encoding){
    x=Hmin(Hmax(x,0.f),1.f);
    if(encoding==0)return x<=.04045f?x/12.92f:Hpow((x+.055f)/1.055f,2.4f);
    return Hpow(x,encoding==1?2.2f:2.4f);
}
// RenoDX ReinhardScalable(channel_max=1, channel_min=0), expressed
// using an input linear-gray/output-nits anchor. Pure white is singular;
// explicitly limit it to the user's HDR peak rather than generate infinity.
inline float InverseReinhard(float y,float grayInput,float grayNits,float peak){
    if(y>=1.f)return peak;
    return Hmin(peak,Hmax(0.f,grayNits*(1.f-grayInput)*y/(grayInput*Hmax(1.f-y,1e-7f))));
}
// RenoDX BT.2446A inverse luma stages. Input is gamma-compressed Y'sdr.
// Clamp the domain and square-root radicand for roundoff/out-of-domain colors.
inline float Bt2446Luma(float ySdr,float sdrNits,float targetNits){
    ySdr=Hmin(Hmax(ySdr,0.f),1.f);
    float ps=1.f+32.f*Hpow(sdrNits/10000.f,1.f/2.4f);
    float yc=Hlog(ySdr*(ps-1.f)+1.f)/Hlog(ps);
    float yp0=yc/1.0770f;
    float yp1=(-2.7811f+Hsqrt(Hmax(0.f,4.83307641f-4.604f*yc)))/-2.302f;
    float yp2=(yc-.5f)/.5f;
    float yp=yp0<=.7399f?yp0:((yp1>.7399f&&yp1<.9909f)?yp1:(yp2>=.9909f?yp2:yp1));
    // Rounded forward segments leave a small gap at the lower knee.
    // Hold that knee across the gap instead of mapping a brighter input darker.
    if(yc>1.0770f*.7399f)yp=Hmax(yp,.7399f);
    float ph=1.f+32.f*Hpow(targetNits/10000.f,1.f/2.4f);
    return (Hpow(ph,yp)-1.f)/(ph-1.f);
}
inline float NeutralNits(float encoded,unsigned int method,unsigned int encoding,float peak,float sdrWhite,float grayInput,float grayNits){
    float y=DecodeSdr(encoded,encoding);
    if(method==0)return InverseReinhard(y,grayInput,grayNits,peak);
    float v=Hmin(Hmax(Bt2446Luma(Hpow(y,1.f/2.4f),sdrWhite,peak),0.f),1.f);
    return Hpow(v,2.4f)*peak;
}
#ifdef __cplusplus
}
#endif
#endif

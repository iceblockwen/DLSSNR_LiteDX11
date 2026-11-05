/*=============================================================================
                                                           
 d8b 888b     d888 888b     d888 8888888888 8888888b.   .d8888b.  8888888888 
 Y8P 8888b   d8888 8888b   d8888 888        888   Y88b d88P  Y88b 888        
     88888b.d88888 88888b.d88888 888        888    888 Y88b.      888        
 888 888Y88888P888 888Y88888P888 8888888    888   d88P  "Y888b.   8888888    
 888 888 Y888P 888 888 Y888P 888 888        8888888P"      "Y88b. 888        
 888 888  Y8P  888 888  Y8P  888 888        888 T88b         "888 888        
 888 888   "   888 888   "   888 888        888  T88b  Y88b  d88P 888        
 888 888       888 888       888 8888888888 888   T88b  "Y8888P"  8888888888                                                                 
                                                                            
    Copyright (c) Pascal Gilcher. All rights reserved.
    

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 DEALINGS IN THE SOFTWARE.

===============================================================================

    Launchpad is a prepass effect that prepares various data to use 
	in later shaders.

    Author:         Pascal Gilcher

    More info:      https://martysmods.com
                    https://patreon.com/mcflypg
                    https://github.com/martymcmodding  	

=============================================================================*/
// Native DX12 adaptation; original matching and regression bodies follow.
// RG16 pixel conversion is only applied at the NR boundary.
cbuffer Constants : register(b0) {
    uint2 TargetSize; uint2 FullSize;
    uint Level; uint ReservedSlots; int Radius; int Dilation;
    uint DepthReverse; uint DepthFlip; uint FirstFrame; uint DepthLog;
    float FarPlane; float Fov; float DepthMultiplier; float Reserved;
    uint2 ColorOrigin,DepthOrigin,DepthSize,MappingPadding;
    float2 DepthScale,DepthOffset,DepthPixelOffset,MappingPad;
};
Texture2D<float4> T0 : register(t0), T1 : register(t1), T2 : register(t2), T3 : register(t3), T4 : register(t4), T5 : register(t5);
// Match uses mip 3 on T3 for pooling and mip 2 on T6 for the level-0 depth key.
Texture2D<float4> T6 : register(t6);
SamplerState LinearMirror : register(s0), LinearClamp : register(s1), PointClamp : register(s2);



struct VSOUT {float4 vpos; float2 uv;};
static const float TAU = 6.283185307179586;
float dot2(float2 v){return dot(v,v);}
bool inside_screen(float2 uv){return all(saturate(uv-uv*uv));}
float2 flow_size(Texture2D<float4> inputTex){uint w,h;inputTex.GetDimensions(w,h);return float2(w,h);}
float3 get_jitter_blue(int2 pos){return T5.Load(int3(pos%256,0)).xyz;}
VSOUT position(uint2 id){VSOUT i;i.vpos=float4(float2(id)+.5,0,1);i.uv=(float2(id)+.5)/TargetSize;return i;}
static float2 kernel[19] = 
{
	float2(0, 0),	
	float2(0.9853, 1.01448),
	float2(-1.37112, 0.34605),
	float2(0.38592, -1.36054),	
	float2(2.15615, -1.16233),
	float2(-0.07147, 2.44845),
	float2(-2.08468, -1.28612),
	float2(-0.684, -3.08742),
	float2(3.01578, 0.95134),
	float2(-2.33178, 2.13607),
	float2(-3.73529, -0.21826),
	float2(2.05667, -3.12572),
	float2(1.67862, 3.34398),
	float2(-1.39388,4.00713),
	float2(-2.77334, -3.2107),
	float2(4.16722, -0.79643),
	float2(3.8414, 2.6914),
	float2(-4.25152, 1.98105),
	float2(0.41012, -4.67245)
};
float4 filter_flow(in VSOUT i, Texture2D<float4> s_flow, const int depth_mip = 3, const int radius = 1, const int dilation = 1)
{	
	float4 center_flow = s_flow.SampleLevel(PointClamp, i.uv, 0);

	//if(DISABLE_POOLING) return center_flow;

	float2 txflow = rcp(flow_size(s_flow));
	float depth = T3.SampleLevel(LinearClamp, i.uv, 0).x; 	

	float3 feature_moments0 = 0;
	float3 feature_moments1 = 0;
	float2 guide_moments = 0;	  
	float3 min_data = 1e10;
	float3 max_data = -1e10;
	float wsum = 1e-8;

	float2 blur_scale = txflow * dilation;

	[loop]for(int y = -radius; y <= radius; y++)
	[loop]for(int x = -radius; x <= radius; x++)
	{		
		float2 tuv = i.uv + blur_scale * float2(x, y);
		float4 tap = s_flow.SampleLevel(PointClamp, tuv, 0);
		if(inside_screen(tuv)) 
		{
			float lw = log2(1.0 + max(0, center_flow.z / (tap.z + 1e-6) - 0.5)); //we want to explicitly get better samples from the neighbours
			float zw = exp(-abs(tap.w / (depth + 1e-6) - 1) * 64.0);
			float w = (lw * zw + 1e-7);

			float g = tap.z;
			feature_moments0 += tap.xyz * w;
			feature_moments1 += tap.xyz * g * w;
			guide_moments += float2(g, g * g) * w;
			wsum += w;

			min_data = min(min_data, tap.xyz);
			max_data = max(max_data, tap.xyz);	
		}	
	}	
	
	feature_moments0 /= wsum;
	feature_moments1 /= wsum;
	guide_moments /= wsum;

	float guide_hi = min_data.z;

	float a00 = 1.0;
	float a01 = guide_moments.x;    
	float a10 = guide_moments.x;
	float a11 = guide_moments.y;  

	a00 += exp2(-14);
	a11 += exp2(-14);
	float idet = rcp(a00 * a11 - a01 * a01); 
	float3 a = idet * (a11 * feature_moments0 - a01 * feature_moments1);
	float3 b = idet * (a00 * feature_moments1 - a10 * feature_moments0);
	float4 res;
	res.xyz = a + b * guide_hi;
	res.xy = clamp(res.xy, min_data.xy, max_data.xy);	
	res.z = guide_hi;
	return float4(res.xyz, T4.SampleLevel(LinearClamp, i.uv + res.xy, 0).x);//write prev frame depth for reprojection validation
}

//can't write to the final flow map when I read it here so
float4 filter_flow_final(in VSOUT i, Texture2D<float4> s_flow, const int depth_mip = 2, const int radius = 3, const int dilation = 1)
{
	//if(DISABLE_UPSCALING) return s_flow.SampleLevel(PointClamp, i.uv, 0);
	float2 txflow = rcp(flow_size(s_flow));
	float depth = T3.SampleLevel(LinearClamp, i.uv, 0).x;

	float3 feature_moments0 = 0;
	float3 feature_moments1 = 0;
	float2 guide_moments = 0;	 
	float wsum = 0;
	float3 min_data = 1e10;
	float3 max_data = -1e10;

	float2 blur_scale = txflow * dilation;

	[loop]for(int y = -radius; y <= radius; y++)
	[loop]for(int x = -radius; x <= radius; x++)
	{
		float2 tuv = i.uv + blur_scale * float2(x, y);
		float4 tap = s_flow.SampleLevel(PointClamp, tuv, 0);
		float ew = inside_screen(tuv);
		float lw = exp2(-tap.z * 4.0); //regular relative weighting
		float zw = exp(-abs(tap.w / (depth + 1e-6) - 1) * 64.0);	
		float w = (lw * zw + 1e-7) * ew; 
		float g = tap.w;
		feature_moments0 += tap.xyz * w;
		feature_moments1 += tap.xyz * g * w;
		guide_moments += float2(g, g * g) * w;
		wsum += w;	
		min_data = min(min_data, tap.xyz);
		max_data = max(max_data, tap.xyz);
	}	

	feature_moments0 /= wsum;
	feature_moments1 /= wsum;
	guide_moments /= wsum;

	float guide_hi = depth;  

	float a00 = 1.0;
	float a01 = guide_moments.x;    
	float a10 = guide_moments.x;
	float a11 = guide_moments.y; 
	a00 += exp2(-18.0);
	a11 += exp2(-18.0);
	float idet = rcp(a00 * a11 - a01 * a01); 
	float3 a = idet * (a11 * feature_moments0 - a01 * feature_moments1);
	float3 b = idet * (a00 * feature_moments1 - a10 * feature_moments0);
	float4 res;
	res.xyz = a + b * guide_hi;
	res.xyz = clamp(res.xyz, min_data, max_data);	
	res.w = depth;
	return res;	
}

#define MATCHING_SAMPLES 16 //absolutely don't tamper with that! It has some internal logic you don't see.

float4 calc_flow_dynamic(VSOUT i,
					 Texture2D<float4> s_feature_curr, 
					 Texture2D<float4> s_feature_prev, 
					 Texture2D<float4> s_flow, 
					 const int level)
{
	float2 motion = 0;
	
	[branch]
	if(level < 7)//if we're not the first pass, do some neighbour pooling to get a better initial guess
	{
		motion = filter_flow(i, s_flow, 3, 1, 1).xy; //dilated, prev pass already filtered without
	}

	const float2 texsize = max(uint2(1, 1), FullSize >> level); 
	const float2 texelsize = rcp(texsize);
	
	float randphi = get_jitter_blue(i.vpos.xy).x;
	float2 sc; sincos(randphi * TAU / 6.0, sc.x, sc.y);
	
	//wrap texelsize scale into the matrix directly. Should resolve with unroll anyhow but might compile faster
	float2x2 km = float2x2(sc.y * texelsize.x, -sc.x * texelsize.y, sc.x * texelsize.x,  sc.y * texelsize.y);
	float mean = 0;	
	float local_block[MATCHING_SAMPLES];

	[unroll]
	for(uint k = 0; k < MATCHING_SAMPLES; k++) 
	{
		float2 tuv = i.uv + mul(kernel[k], km);
		//actually need that, precision issue					
		float4 texels = s_feature_curr.GatherRed(LinearMirror, tuv);
		float2 t = frac(tuv * texsize - 0.5);
		float2 a = mad(texels.zy - texels.wx, t.xx, texels.wx);
		local_block[k] = mad(a.y - a.x, t.y, a.x);		
		mean += local_block[k];
	}

	mean /= MATCHING_SAMPLES;
	float MAD = 0;
	[unroll]
	for(uint k = 0; k < MATCHING_SAMPLES; k++)
		MAD += abs(mean - local_block[k]);	

	int num_steps = 4 * (level + 1);
	num_steps = min(num_steps, 32);
	num_steps = MAD < 1.0/255.0 ? 1 : num_steps;

	float2 best_motion = motion;
	float  best_loss = 1e10;

	[loop]
	for(int j = 0; j < num_steps; j++)
	{	
		float loss = 0;
		float2 grad = 0;

		[unroll]
		for(uint k = 0; k < MATCHING_SAMPLES; k++)
		{
			float2 tuv = i.uv + motion + mul(kernel[k], km);	
    		float4 texels = s_feature_prev.GatherRed(LinearMirror, tuv);
			float2 t = frac(tuv * texsize - 0.5);

			float3 d = texels.zyx - texels.wxw;
			float2 dfduv = d.xz + (d.y - d.x) * t.yx;	
			float f = d.z * t.y + (dfduv.x * t.x + texels.w);
		
			float tloss = f - local_block[k];	
			grad += tloss > 0.0 ? dfduv : -dfduv;
			loss += abs(tloss);
		}	
 
		[branch]
		if(loss < best_loss * 0.9999)
		{
			best_loss = loss;
			best_motion = motion;		
		}
		else 
		{	
			j += 2;
		}

		grad *= texsize;			
		float2 gradstep = grad / (1e-15 + dot(grad, grad)) * loss;
		gradstep *= saturate(0.5 * rsqrt(1e-8 + dot2(gradstep * texsize)));	
		motion -= gradstep;	
	}

	float depth_key = 0;

	[branch]
	if(level == 0) //upscaling should be bilateral on curr frame depth, more accurate
	{
		depth_key = T6.SampleLevel(LinearClamp, i.uv, 0).x; //2 -> upscale
	}
	else //vector pooling makes more sense to measure prev frame reprojection error, less flickery
	{
		depth_key = T4.SampleLevel(LinearClamp, i.uv + motion, 0).x;
	}
	
	best_loss = best_loss / (0.01 + MAD);		
	best_loss += saturate(1 - MAD * 255.0) * 0.5; 	//do not touch! good weight for the bilateral upscale filter.

	float4 curr_layer = float4(best_motion, best_loss, depth_key);
	return curr_layer;
}



void downsample_features(Texture2D<float4> s0, Texture2D<float4> s1, float2 uv, out float4 f0, out float f1)
{
	float2 tx = rcp(flow_size(s0));
	f0 = f1 = 0;
	f0 += s0.SampleLevel(LinearMirror, uv + float2(-1.90313, -1.90313) * tx, 0) * 0.083411; 
	f0 += s0.SampleLevel(LinearMirror, uv + float2(       0, -1.90313) * tx, 0) * 0.121987; 
	f0 += s0.SampleLevel(LinearMirror, uv + float2( 1.90313, -1.90313) * tx, 0) * 0.083411; 
	f0 += s0.SampleLevel(LinearMirror, uv + float2(-1.90313,        0) * tx, 0) * 0.121987; 
	f0 += s0.SampleLevel(LinearMirror, uv + float2(       0,        0) * tx, 0) * 0.178404; 
	f0 += s0.SampleLevel(LinearMirror, uv + float2( 1.90313,        0) * tx, 0) * 0.121987; 
	f0 += s0.SampleLevel(LinearMirror, uv + float2(-1.90313,  1.90313) * tx, 0) * 0.083411; 
	f0 += s0.SampleLevel(LinearMirror, uv + float2(       0,  1.90313) * tx, 0) * 0.121987; 
	f0 += s0.SampleLevel(LinearMirror, uv + float2( 1.90313,  1.90313) * tx, 0) * 0.083411;
	f1 += s1.SampleLevel(LinearMirror, uv + float2(-1.90313, -1.90313) * tx, 0).x * 0.083411; 
	f1 += s1.SampleLevel(LinearMirror, uv + float2(       0, -1.90313) * tx, 0).x * 0.121987; 
	f1 += s1.SampleLevel(LinearMirror, uv + float2( 1.90313, -1.90313) * tx, 0).x * 0.083411; 
	f1 += s1.SampleLevel(LinearMirror, uv + float2(-1.90313,        0) * tx, 0).x * 0.121987; 
	f1 += s1.SampleLevel(LinearMirror, uv + float2(       0,        0) * tx, 0).x * 0.178404; 
	f1 += s1.SampleLevel(LinearMirror, uv + float2( 1.90313,        0) * tx, 0).x * 0.121987; 
	f1 += s1.SampleLevel(LinearMirror, uv + float2(-1.90313,  1.90313) * tx, 0).x * 0.083411; 
	f1 += s1.SampleLevel(LinearMirror, uv + float2(       0,  1.90313) * tx, 0).x * 0.121987; 
	f1 += s1.SampleLevel(LinearMirror, uv + float2( 1.90313,  1.90313) * tx, 0).x * 0.083411;
}


float4 FlowVS(uint id:SV_VertexID):SV_Position {
    float2 uv=float2((id<<1)&2,id&2);return float4(uv*float2(2,-2)+float2(-1,1),0,1);
}
void PSPrepare(float4 pos:SV_Position,out float feature:SV_Target0,out float depth:SV_Target1) {
    float2 uv=pos.xy/TargetSize,duv=uv;if(DepthFlip)duv.y=1-duv.y;
    feature=dot(.3333,T0.Load(int3(int2(pos.xy)+int2(ColorOrigin),0)).rgb)+.05;
    duv/=DepthScale;
    duv.x-=DepthPixelOffset.x!=0?DepthPixelOffset.x/FullSize.x:DepthOffset.x/2.000000001;
    duv.y+=DepthPixelOffset.y!=0?DepthPixelOffset.y/FullSize.y:DepthOffset.y/2.000000001;
    uint dw,dh;T1.GetDimensions(dw,dh);
    float2 pixel=clamp(DepthOrigin+duv*DepthSize,DepthOrigin+.5,DepthOrigin+DepthSize-.5);
    float z=T1.SampleLevel(LinearClamp,pixel/float2(dw,dh),0).x*DepthMultiplier;
    if(DepthLog)z*=lerp(z,1,.04975);if(DepthReverse)z=1-z;
    z=saturate(z/(FarPlane-z*(FarPlane-1)))*FarPlane+1;
    float2 add=-tan(radians(Fov)*.5)*float2(float(FullSize.x)/FullSize.y,1);
    depth=length(float3((uv*(-2*add)+add)*z,z)/FarPlane);
}
float PSDepthMip(float4 pos:SV_Position):SV_Target0 {
    // Match the normalized linear downsample used by ReShade's GenerateMips.
    // Each R16 target rounds before the next level samples it.
    return T0.SampleLevel(LinearClamp,pos.xy/TargetSize,0).x;
}
void PSDown(float4 pos:SV_Position,out float feature:SV_Target0,out float previous:SV_Target1) {
    float4 f0;downsample_features(T0,T1,pos.xy/TargetSize,f0,previous);feature=f0.x;
}
float4 PSMatch(float4 pos:SV_Position):SV_Target0 {return calc_flow_dynamic(position(uint2(pos.xy)),T0,T1,T2,Level);}
float4 PSPool(float4 pos:SV_Position):SV_Target0 {return filter_flow(position(uint2(pos.xy)),T2,3,Radius,Dilation);}
float4 PSUpscale(float4 pos:SV_Position):SV_Target0 {return filter_flow_final(position(uint2(pos.xy)),T2,Level,Radius,Dilation);}
float4 PSFinish(float4 pos:SV_Position):SV_Target0 {
    float4 value=filter_flow_final(position(uint2(pos.xy)),T2,0,1,2);
    return value;
}

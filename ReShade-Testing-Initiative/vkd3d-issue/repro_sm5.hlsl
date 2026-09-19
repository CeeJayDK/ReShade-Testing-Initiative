// Same restriction at ps_5_0 -- shows the check is not shader-model gated.
struct Sampler2D { Texture2D<float4> t; SamplerState s; float2 pixelsize; };

Texture2D<float4> backbuffer_t : register(t0);
SamplerState backbuffer_ss : register(s0);
static const Sampler2D backbuffer = { backbuffer_t, backbuffer_ss, float2(0.001, 0.002) };

float4 main(float2 uv : TEXCOORD) : SV_Target
{
    return backbuffer.t.Sample(backbuffer.s, uv + backbuffer.pixelsize);
}

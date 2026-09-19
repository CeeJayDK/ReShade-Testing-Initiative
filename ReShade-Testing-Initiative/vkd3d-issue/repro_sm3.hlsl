// vkd3d-shader E5002 repro -- compile as ps_3_0
// Expected: compiles (D3DCompiler accepts this)
// Actual:   E5002: Static variables cannot have both numeric and resource components.
struct Sampler2D { sampler2D s; float2 pixelsize; };

sampler2D backbuffer_s : register(s0);
static const Sampler2D backbuffer = { backbuffer_s, float2(0.001, 0.002) };

float4 main(float2 uv : TEXCOORD) : COLOR
{
    return tex2D(backbuffer.s, uv + backbuffer.pixelsize);
}

// Compiles cleanly: the numeric member split out of the struct.
struct Sampler2D { sampler2D s; };

sampler2D backbuffer_s : register(s0);
static const Sampler2D backbuffer = { backbuffer_s };
static const float2 backbuffer_pixelsize = float2(0.001, 0.002);

float4 main(float2 uv : TEXCOORD) : COLOR
{
    return tex2D(backbuffer.s, uv + backbuffer_pixelsize);
}

#include "ReShade.fxh"
float4 BPS(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	float3 c = tex2D(ReShade::BackBuffer, uv).rgb
	return float4(c * undefined_var, 1);
}
technique Broken { pass { VertexShader = PostProcessVS; PixelShader = BPS; } }

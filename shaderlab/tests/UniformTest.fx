#include "ReShade.fxh"
uniform float u_a < ui_type = "slider"; > = 0.1;
uniform float u_b < ui_type = "slider"; > = 0.2;
uniform int   u_c < ui_type = "slider"; > = 3;
uniform float u_d < ui_type = "slider"; > = 0.4;
uniform bool  u_e < > = true;
uniform float2 u_f < > = float2(0.6, 0.7);
float4 UPS(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	int i = int(pos.x) / 10;
	float v = 0;
	if (i == 0) v = u_a; if (i == 1) v = u_b; if (i == 2) v = u_c / 10.0; if (i == 3) v = u_d;
	if (i == 4) v = u_e ? 0.5 : 0.0; if (i == 5) v = u_f.x; if (i == 6) v = u_f.y;
	return float4(v, v, v, 1);
}
technique UniformTest { pass { VertexShader = PostProcessVS; PixelShader = UPS; } }

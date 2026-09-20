#include "ReShade.fxh"
uniform int mode < ui_type = "combo"; > = 0;
sampler sLin { Texture = ReShade::BackBufferTex; SRGBTexture = true; };
float4 PS(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return tex2D(sLin, uv); }
float4 PS2(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return tex2D(ReShade::BackBuffer, uv); }
technique SRGBRoundTrip { pass { VertexShader = PostProcessVS; PixelShader = PS; SRGBWriteEnable = true; } }
technique SRGBReadOnly  { pass { VertexShader = PostProcessVS; PixelShader = PS; } }
technique SRGBWriteOnly { pass { VertexShader = PostProcessVS; PixelShader = PS2; SRGBWriteEnable = true; } }

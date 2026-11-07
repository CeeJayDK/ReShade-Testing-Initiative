// Constructs vkd3d-shader rejects and cli/vkd3d_hlsl_fixups.hpp rewrites.
// Each must compile with reshadefx_cli_fixed --dxbc on Linux:
//   reshadefx_cli_fixed --dxbc --shader-model 50 --all-entry-points -Fo out.cso cli/tests/vkd3d_fixups.fx
// The intrinsics sit in a function other than the entry point, where vkd3d
// cannot compile isinf, and in loops, which ReShade marks [fastopt].

float4 PostProcessVS(uint id : SV_VertexID, out float2 texcoord : TEXCOORD) : SV_Position
{
	texcoord = float2(id == 2 ? 2.0 : 0.0, id == 1 ? 2.0 : 0.0);
	return float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 classify(float2 p)
{
	float x = 1.0 / (p.x - 100.0);
	float2 v = 1.0 / (p - 100.0);
	float4 result = float4(isinf(x), isnan(x - x), isinf(v));
	[loop] for (int i = 0; i < int(p.y) % 4; i++)
		result.x += float(isinf(x * float(i)));
	return result;
}

float4 reverse(float2 p)
{
	uint u = uint(p.x);
	uint3 w = uint3(p.xyx);
	int i = int(p.y);
	// ReShade passes a cast here: reversebits(((uint)i))
	return float4(float(reversebits(u) >> 28) + float(reversebits(uint(i)) >> 30), float3(reversebits(w) >> 31));
}

float4 PS_Classify(float4 pos : SV_Position) : SV_Target { return classify(pos.xy); }
float4 PS_Reverse(float4 pos : SV_Position) : SV_Target { return reverse(pos.xy); }

technique VKD3DFixups
{
	pass { VertexShader = PostProcessVS; PixelShader = PS_Classify; }
	pass { VertexShader = PostProcessVS; PixelShader = PS_Reverse; }
}

#include <stdio.h>
#include <string.h>
#include "vkd3d_shader.h"

/* Same mixed static, but SM4-style resources and a ps_5_0 profile. */
static const char *src =
"struct Sampler2D { Texture2D<float4> t; SamplerState s; float2 pixelsize; };\n"
"\n"
"Texture2D<float4> backbuffer_t : register(t0);\n"
"SamplerState backbuffer_ss : register(s0);\n"
"static const Sampler2D backbuffer = { backbuffer_t, backbuffer_ss, float2(0.001, 0.002) };\n"
"\n"
"float4 main(float2 uv : TEXCOORD) : SV_Target\n"
"{\n"
"    return backbuffer.t.Sample(backbuffer.s, uv + backbuffer.pixelsize);\n"
"}\n";

int main(void)
{
    struct vkd3d_shader_compile_info info = {0};
    struct vkd3d_shader_hlsl_source_info hlsl = {0};
    struct vkd3d_shader_code out = {0};
    char *messages = NULL;

    hlsl.type = VKD3D_SHADER_STRUCTURE_TYPE_HLSL_SOURCE_INFO;
    hlsl.entry_point = "main";
    hlsl.profile = "ps_5_0";

    info.type = VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;
    info.next = &hlsl;
    info.source.code = src;
    info.source.size = strlen(src);
    info.source_type = VKD3D_SHADER_SOURCE_HLSL;
    info.target_type = VKD3D_SHADER_TARGET_DXBC_TPF;
    info.log_level = VKD3D_SHADER_LOG_WARNING;

    int rc = vkd3d_shader_compile(&info, &out, &messages);
    printf("ps_5_0 with mixed static: rc=%d\n%s\n", rc, messages ? messages : "(no messages)");
    return 0;
}

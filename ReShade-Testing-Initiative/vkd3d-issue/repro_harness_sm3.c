#include <stdio.h>
#include <string.h>
#include "vkd3d_shader.h"

static const char *src =
"struct Sampler2D { sampler2D s; float2 pixelsize; };\n"
"\n"
"sampler2D backbuffer_s : register(s0);\n"
"static const Sampler2D backbuffer = { backbuffer_s, float2(0.001, 0.002) };\n"
"\n"
"float4 main(float2 uv : TEXCOORD) : COLOR\n"
"{\n"
"    return tex2D(backbuffer.s, uv + backbuffer.pixelsize);\n"
"}\n";

int main(void)
{
    struct vkd3d_shader_compile_info info = {0};
    struct vkd3d_shader_hlsl_source_info hlsl = {0};
    struct vkd3d_shader_compile_option opts[1];
    struct vkd3d_shader_code out = {0};
    char *messages = NULL;

    opts[0].name = VKD3D_SHADER_COMPILE_OPTION_BACKWARD_COMPATIBILITY;
    opts[0].value = VKD3D_SHADER_COMPILE_OPTION_BACKCOMPAT_MAP_SEMANTIC_NAMES;

    hlsl.type = VKD3D_SHADER_STRUCTURE_TYPE_HLSL_SOURCE_INFO;
    hlsl.entry_point = "main";
    hlsl.profile = "ps_3_0";

    info.type = VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;
    info.next = &hlsl;
    info.source.code = src;
    info.source.size = strlen(src);
    info.source_type = VKD3D_SHADER_SOURCE_HLSL;
    info.target_type = VKD3D_SHADER_TARGET_D3D_BYTECODE;
    info.options = opts;
    info.option_count = 1;
    info.log_level = VKD3D_SHADER_LOG_WARNING;

    int rc = vkd3d_shader_compile(&info, &out, &messages);
    printf("ps_3_0: rc=%d\n%s\n", rc, messages ? messages : "(no messages)");
    return 0;
}

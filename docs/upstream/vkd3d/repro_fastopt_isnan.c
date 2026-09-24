/* vkd3d-shader: [fastopt] (E5017) and isnan (E5005). Build against libvkd3d-shader, e.g.
   gcc repro_fastopt_isnan.c -I <vkd3d>/include -L<lib> -lvkd3d-shader -o repro */
#include <stdio.h>
#include <string.h>
#include "vkd3d_shader.h"

static void compile(const char *name, const char *src)
{
    struct vkd3d_shader_compile_info info = {0};
    struct vkd3d_shader_hlsl_source_info hlsl = {0};
    struct vkd3d_shader_code out = {0};
    char *messages = NULL;
    int rc;

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
    rc = vkd3d_shader_compile(&info, &out, &messages);
    printf("%s: rc=%d\n%s\n", name, rc, messages ? messages : "(no messages)");
}

int main(void)
{
    compile("fastopt",
        "float4 main(float4 pos : SV_Position) : SV_Target\n"
        "{\n"
        "    float s = 0;\n"
        "    [fastopt] for (int i = 0; i < 4; ++i)\n"
        "        s += pos.x * i;\n"
        "    return s;\n"
        "}\n");
    compile("isnan",
        "float4 main(float4 pos : SV_Position) : SV_Target\n"
        "{\n"
        "    return isnan(pos.x) ? 0 : 1;\n"
        "}\n");
    return 0;
}

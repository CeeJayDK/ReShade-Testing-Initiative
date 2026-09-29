/* vkd3d-shader: integer division by a constant zero in dead code (E5021). Build
   against libvkd3d-shader, e.g.
   gcc repro_divzero.c -I <vkd3d>/include -L<lib> -lvkd3d-shader -o repro */
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
    /* The modulo is guarded, so it never divides by zero. After unrolling, the
       first iteration has cycle == 0 and the guarded block is dead, but the
       modulo in it is constant-folded first, and that is a hard error. */
    compile("guarded modulo in an unrolled loop",
        "float4 main(float4 pos : SV_Position) : SV_Target\n"
        "{\n"
        "    int cycle = 0, n = 0;\n"
        "    float sum = 0;\n"
        "    [unroll] for (int i = 0; i < 4; i++)\n"
        "    {\n"
        "        if (cycle != 0)\n"
        "        {\n"
        "            if (n % cycle == 0)\n"
        "                sum += pos.x;\n"
        "        }\n"
        "        cycle += 2;\n"
        "        n++;\n"
        "    }\n"
        "    return sum;\n"
        "}\n");
    return 0;
}

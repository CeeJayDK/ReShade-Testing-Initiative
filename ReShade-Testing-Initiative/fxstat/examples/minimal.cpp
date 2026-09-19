/*
 * The smallest thing that uses libfxstat: compile an effect from an in-memory
 * string and print the texture fetch count. No files, no CLI, no printf inside
 * the library -- this is the shape a WASM binding wraps.
 *
 * build:
 *   g++ -std=c++17 -I include -I <reshade>/source examples/minimal.cpp \
 *       build/libfxstat_core.a build/libreshadefx.a \
 *       -lSPIRV-Tools-opt -lSPIRV-Tools -o minimal
 */
#include "fxstat/fxstat.hpp"
#include <cstdio>

static const char *effect = R"(
uniform float strength < ui_min = 0.0; ui_max = 2.0; > = 1.0;
uniform int taps < ui_min = 1; ui_max = 3; > = 1;

texture BackBufferTex : COLOR;
sampler BackBuffer { Texture = BackBufferTex; };

float4 PS(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    float3 c = tex2D(BackBuffer, uv).rgb;
    if (taps > 1)
        c += tex2D(BackBuffer, uv + float2(0.001, 0)).rgb;
    if (taps > 2)
        c += tex2D(BackBuffer, uv - float2(0.001, 0)).rgb;
    return float4(c * strength, 1.0);
}

technique Example { pass { VertexShader = PostProcessVS; PixelShader = PS; } }
)";

// A minimal PostProcessVS so the example needs no include path.
static const char *prelude = R"(
void PostProcessVS(in uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD)
{
    uv.x = (id == 2) ? 2.0 : 0.0;
    uv.y = (id == 1) ? 2.0 : 0.0;
    pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
)";

int main()
{
    const std::string source = std::string(prelude) + effect;

    for (const char *taps : { "1", "2", "3" })
    {
        fxstat::compile_options options;
        options.target = fxstat::backend::spirv;
        options.performance_mode = true;
        options.preset["taps"] = taps;
        options.preset["strength"] = "1.5";

        const fxstat::compile_result r = fxstat::compile_source(source, "Example.fx", options);
        if (!r.ok)
        {
            std::fprintf(stderr, "taps=%s failed:\n%s", taps, r.errors.c_str());
            return 1;
        }

        for (const fxstat::entry_point_result &e : r.entry_points)
            if (e.stage == "pixel")
                std::printf("taps=%s  ->  TEX %u, ALU %u, total %u\n",
                            taps, e.stats.get(fxstat::category::tex),
                            e.stats.get(fxstat::category::alu), e.stats.total);
    }
    return 0;
}

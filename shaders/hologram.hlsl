// A hologram: rim light, world-space scan bands, scanlines, glitch flicker.
//   param1.xyz colour (default cyan)
#include "weaponfx.hlsli"
#include "effects.hlsli"

float4 main(PSIn i) : SV_Target {
    float3 tint = dot(g_fxParam[1].xyz, 1.0) > 0.0 ? g_fxParam[1].xyz : float3(0.2, 0.9, 1.0);
    return float4(Hologram(i, tint), 1.0);
}

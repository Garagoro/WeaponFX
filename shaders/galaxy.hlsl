// A window into space: starfield and nebula seen through the weapon.
//   param0.x  nebula strength (0 = default 1)
//   param0.y  rim glow strength (0 = default 1)
#include "weaponfx.hlsli"
#include "effects.hlsli"

float4 main(PSIn i) : SV_Target {
    float nebulaK = g_fxParam[0].x > 0.0 ? g_fxParam[0].x : 1.0;
    float rimK    = g_fxParam[0].y > 0.0 ? g_fxParam[0].y : 1.0;
    return float4(Galaxy(i, nebulaK, rimK), 1.0);
}

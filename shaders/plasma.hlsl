// Molten energy flowing along the weapon's UV, with hot veins and a pulse.
//   param0.x  flow speed (default 1)
//   param0.y  vein scale (default 1)
#include "weaponfx.hlsli"
#include "effects.hlsli"

float4 main(PSIn i) : SV_Target {
    float speed = g_fxParam[0].x > 0.0 ? g_fxParam[0].x : 1.0;
    float scale = g_fxParam[0].y > 0.0 ? g_fxParam[0].y : 1.0;
    return float4(Plasma(i, speed, scale), 1.0);
}

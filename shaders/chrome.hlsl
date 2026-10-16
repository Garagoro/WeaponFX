// Polished metal: the map's environment reflected, with a sheen sweeping along.
//   param0.x  probe slice (default 0)
//   param0.y  blur mip (default 0 = sharp)
//   param1.xyz tint (default white)
#include "weaponfx.hlsli"
#include "effects.hlsli"

float4 main(PSIn i) : SV_Target {
    float3 tint = dot(g_fxParam[1].xyz, 1.0) > 0.0 ? g_fxParam[1].xyz : float3(1, 1, 1);
    return float4(Chrome(i, g_fxParam[0].x, g_fxParam[0].y, tint), 1.0);
}

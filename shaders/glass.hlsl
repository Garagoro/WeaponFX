// Clear glass: the world behind the weapon, bent by its surface, with a prism
// fringe and a thin environment reflection at grazing angles.
//   param0.x  refraction in pixels (default 40)
//   param0.y  dispersion, fraction of the offset (default 0.15)
//   param0.z  environment probe slice (default 0)
//   param1.xyz tint (default faint blue)
#include "weaponfx.hlsli"
#include "effects.hlsli"

float4 main(PSIn i) : SV_Target {
    float px   = g_fxParam[0].x > 0.0 ? g_fxParam[0].x : 40.0;
    float disp = g_fxParam[0].y > 0.0 ? g_fxParam[0].y : 0.15;
    float3 tint = dot(g_fxParam[1].xyz, 1.0) > 0.0 ? g_fxParam[1].xyz : float3(0.85, 0.95, 1.0);
    return float4(Glass(i, px, disp, tint, g_fxParam[0].z), 1.0);
}

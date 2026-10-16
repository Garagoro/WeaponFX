// Probe: is the scene copy bound, and what does it hold under the weapon.
//   magenta  g_fxScene unbound (no copy this draw)
//   red      bound, exactly black here
//   blue     g_fxDepth unbound (only checked where the scene is fine)
//   else     the copy itself
#include "weaponfx.hlsli"

float4 main(PSIn i) : SV_Target {
    float w, h;
    g_fxScene.GetDimensions(w, h);
    if (w < 1.0) return float4(1, 0, 1, 1);
    float3 c = SceneAt(i, float2(0, 0));
    if (dot(c, c) < 1e-6) return float4(1, 0, 0, 1);
    if (!HasDepth()) return float4(0, 0, 1, 1);
    return float4(c, 1);
}

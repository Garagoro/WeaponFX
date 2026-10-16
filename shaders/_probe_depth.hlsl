// Probe: what the world's depth copy (t37) holds under the weapon.
//   red     closer than 64 units - something near the eye wrote depth here
//   green   bands every 256 units - ordinary world depth
//   blue    the far plane (raw 0 or 1) - nothing wrote depth here
//   magenta no depth copy bound
#include "weaponfx.hlsli"

float4 main(PSIn i) : SV_Target {
    if (!HasDepth()) return float4(1, 0, 1, 1);
    float w, h;
    g_fxDepth.GetDimensions(w, h);
    float raw = g_fxDepth.Load(int3(clamp(i.svpos.xy, float2(0, 0), float2(w, h) - 1.0), 0));
    if (raw <= 0.0 || raw >= 1.0) return float4(0, 0, 1, 1);
    float d = SceneDistance(i.svpos.xy);
    if (d < 64.0) return float4(1, 0, 0, 1);
    return float4(0, frac(d / 256.0) * 0.8 + 0.2, 0, 1);
}

// Two effects split by a mask drawn on the weapon's UV (g_fxTex0, red
// channel): chrome where the mask is black, a window into space where it is
// white, the edge soft so a blurry brush stroke blends. Without a mask the
// texture reads 0 and the whole weapon is chrome.
//   tex0      the mask (weaponfx.ini)
//   param0.x  edge softness, mask units (default 0.08)
#include "weaponfx.hlsli"
#include "effects.hlsli"

float4 main(PSIn i) : SV_Target {
    float soft = g_fxParam[0].x > 0.0 ? g_fxParam[0].x : 0.08;
    float m = smoothstep(0.5 - soft, 0.5 + soft, Tex0(i).r);
    float3 a = Chrome(i, 0.0, 0.0, float3(1, 1, 1));
    float3 b = Galaxy(i, 1.0, 1.0);
    // A thin glowing seam where the two meet.
    float seam = 1.0 - abs(m * 2.0 - 1.0);
    return float4(lerp(a, b, m) + seam * seam * float3(0.4, 0.6, 1.0), 1.0);
}

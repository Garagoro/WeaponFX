// Seeing through the weapon: where the mask is white the weapon is gone and the
// world behind it shows, bent by the surface like a lens; where it is black
// the weapon's own picture stays (the output is premultiplied alpha over it).
//
// Two ways to bend, param0.z:
//   depth-aware (default): the ray turns by an angle at the surface, so what
//     is far behind moves a lot and what touches the weapon barely moves -
//     a wall right behind the blade stays put, the street beyond swims.
//   flat (param0.z < 0): a plain screen shift, the same for everything.
//
//   tex0      mask by the weapon's UV, red channel (weaponfx.ini)
//   param0.x  amount where there is no mask texture (default 1 = all of it)
//   param0.y  bend, radians at a surface facing fully sideways (default 0.04;
//             negative = no bend, a clean hole)
//   param0.z  < 0 = flat bend instead of depth-aware
//   param0.w  rim glow (default 0.1; negative = none)
//   param1.x  dispersion, fraction of the bend (default 0 = none)
#include "weaponfx.hlsli"

float4 main(PSIn i) : SV_Target {
    float m = HasTex0() ? Tex0(i).r : (g_fxParam[0].x > 0.0 ? g_fxParam[0].x : 1.0);
    m = saturate(m);
    float bend = g_fxParam[0].y == 0.0 ? 0.04 : max(g_fxParam[0].y, 0.0);
    float glow = g_fxParam[0].w == 0.0 ? 0.10 : max(g_fxParam[0].w, 0.0);
    float disp = max(g_fxParam[1].x, 0.0);

    // The full shift: what infinitely far away moves by.
    float2 full = ScreenNormal(i) * bend * FocalPx();
    float2 off = full;
    if (g_fxParam[0].z >= 0.0 && HasDepth()) {
        // Seen from the eye, a point at distance D behind a lens at distance dW
        // moves by the bend angle times (1 - dW / D). D is read where the bent
        // ray lands, so a few rounds settle it at depth edges.
        float dW = PixelDistance(i);
        [unroll] for (int k = 0; k < 3; ++k) {
            float D = SceneDistance(i.svpos.xy + off);
            off = full * saturate(1.0 - dW / max(D, dW + 1e-3));
        }
    }
    float3 seen;
    seen.r = SceneAt(i, off * (1.0 + disp)).r;
    seen.g = SceneAt(i, off).g;
    seen.b = SceneAt(i, off * (1.0 - disp)).b;
    // The bent read can land behind a hand drawn after the copy: the world is
    // never drawn there (the hands are in the depth prepass), the copy holds
    // pure black, and a rifle near the fingers showed black streaks. Unbent,
    // the read stays under the weapon itself.
    if (dot(seen, seen) < 1e-6) seen = SceneAt(i, float2(0, 0));

    // A thin outline so the weapon reads as there, plus a line along the mask
    // edge where see-through meets the weapon's own picture.
    float rim = Fresnel(i, 4.0) * glow;
    float edge = saturate(fwidth(m) * 3.0) * glow * 4.0;
    float3 col = seen + (rim + edge) * float3(0.6, 0.8, 1.0);
    return float4(col * m, m);
}

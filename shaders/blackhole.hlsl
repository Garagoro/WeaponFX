// A black hole inside the weapon: the weapon is a clear shell, and through it
// the world bends around a point of nothing - pulled into an Einstein ring,
// with a black horizon, a thin bright photon ring and a hot accretion disk
// whirling around it, brighter on the side that comes towards you.
//
// The hole sits at a fixed spot on the screen (the viewmodel is fixed to the
// camera, so it stays inside the weapon); during the inspect animation the
// weapon moves away from it. Put it where your weapon is:
//   param0.xy  centre, fraction of the screen (default 0.66 0.74)
//   param0.z   horizon radius, fraction of the screen height (default 0.02)
//   param0.w   lensing: Einstein radius in horizon radii (default 2.6)
//   param1.x   disk brightness (default 1; negative = no disk)
//   param1.y   disk tilt: 0 edge-on .. 1 face-on (default 0.25)
//   param1.z   spin speed (default 1)
#include "weaponfx.hlsli"

float4 main(PSIn i) : SV_Target {
    float2 c01  = g_fxParam[0].x > 0.0 ? g_fxParam[0].xy : float2(0.66, 0.74);
    float rh01  = g_fxParam[0].z > 0.0 ? g_fxParam[0].z : 0.02;
    float ein   = g_fxParam[0].w > 0.0 ? g_fxParam[0].w : 2.6;
    float diskK = g_fxParam[1].x == 0.0 ? 1.0 : max(g_fxParam[1].x, 0.0);
    float tilt  = g_fxParam[1].y > 0.0 ? saturate(g_fxParam[1].y) : 0.25;
    float spin  = g_fxParam[1].z > 0.0 ? g_fxParam[1].z : 1.0;

    float2 centre = c01 * g_fxScreen.xy;
    float rh = rh01 * g_fxScreen.y;          // horizon, pixels
    float re = rh * ein;                     // Einstein radius, pixels
    float2 d = i.svpos.xy - centre;
    float r = max(length(d), 1e-3);
    float2 dir = d / r;

    // Point-lens mapping: a pixel at r shows what is at r - re^2 / r. Outside
    // the Einstein ring the world is pushed out; inside it the far side of the
    // world is folded in, flipped.
    float src = r - re * re / r;
    float2 off = dir * (src - r);
    float3 seen = SceneAt(i, off);

    // The weapon itself: just a glass edge, so it still reads as a weapon.
    float fr = Fresnel(i, 5.0);
    float3 col = seen + fr * 0.15 * float3(0.7, 0.8, 1.0);

    // The accretion disk: a ring in a tilted plane, so on screen an ellipse
    // squashed vertically. Swirling noise in the disk's own angle and radius,
    // white-hot inside, orange outside, and Doppler-bright on the left side.
    float3 diskCol = 0;
    float diskA = 0;
    if (diskK > 0.0) {
        float2 e = float2(d.x, d.y / max(tilt, 0.05));
        float er = length(e) / rh;
        float ang = atan2(e.y, e.x);
        float inner = 1.6, outer = 4.5;
        float band = smoothstep(inner, inner + 0.3, er) * smoothstep(outer, outer - 1.5, er);
        float swirl = Fbm3(float3(ang * 3.0 + Time() * 1.5 * spin - er * 1.2, er * 2.0, Time() * 0.2));
        float heat = saturate(1.0 - (er - inner) / (outer - inner));
        float3 hot = lerp(float3(1.0, 0.35, 0.08), float3(1.0, 0.9, 0.75), heat * heat);
        float doppler = 0.55 + 0.45 * (-e.x / max(length(e), 1e-3));
        diskCol = hot * (0.4 + 1.6 * swirl) * band * doppler * 2.2 * diskK;
        diskA = band * saturate(0.6 + swirl);
        // The disk passes in front of the hole only on its near (lower) half;
        // the far half is hidden behind the horizon.
        if (d.y < 0.0) diskA *= smoothstep(rh * 0.9, rh * 1.1, r);
    }

    // The lensed image of the disk's far side, bent up over the hole: a thin
    // arc hugging the photon ring - what makes it read as a black hole.
    float arc = smoothstep(0.25, 0.0, abs(r / rh - 1.5)) * diskK;
    float3 arcCol = float3(1.0, 0.75, 0.45) * arc * 1.4 *
                    (0.7 + 0.6 * Fbm3(float3(atan2(d.y, d.x) * 4.0 - Time() * spin, 0.0, 0.0)));

    // The horizon: black, with the photon ring a hair outside it.
    float ring = smoothstep(0.12, 0.0, abs(r / rh - 1.05));
    float hole = smoothstep(1.0, 0.97, r / rh);

    col = lerp(col, diskCol, saturate(diskA));
    col += arcCol * (1.0 - hole);
    col += ring * float3(1.0, 0.85, 0.6) * 2.0;
    col = lerp(col, 0.0, hole);
    return float4(col, 1.0);
}

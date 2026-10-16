// Active camouflage, the Predator kind: the weapon is all but invisible - the
// world behind it shows through, rippling and bent by a shimmer that crawls
// over the surface - and only its outline gives it away: a thin iridescent
// edge with electric crackle running along it.
//   param0.x  shimmer, pixels of displacement (default 10)
//   param0.y  outline strength (default 1; negative = none)
//   param0.z  crackle strength (default 1; negative = none)
//   param0.w  shimmer speed (default 1)
//   param1.x  how visible the body is, 0..1 (default 0.06): a faint tint of the
//             outline colour over the whole weapon
#include "weaponfx.hlsli"

// Hue wheel, for the oily edge that shifts colour with the viewing angle.
float3 Hue(float h) {
    return saturate(abs(frac(h + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0);
}

float4 main(PSIn i) : SV_Target {
    float px     = g_fxParam[0].x > 0.0 ? g_fxParam[0].x : 10.0;
    float rimK   = g_fxParam[0].y == 0.0 ? 1.0 : max(g_fxParam[0].y, 0.0);
    float crackK = g_fxParam[0].z == 0.0 ? 1.0 : max(g_fxParam[0].z, 0.0);
    float speed  = g_fxParam[0].w > 0.0 ? g_fxParam[0].w : 1.0;
    float body   = g_fxParam[1].x > 0.0 ? saturate(g_fxParam[1].x) : 0.06;
    float t = Time() * speed;

    // The shimmer lives in the weapon's UV, so it sticks to the weapon and
    // crawls over it instead of swimming across the screen. Two noise fields
    // give a displacement vector; ripples running along the weapon on top.
    float3 p = float3(i.uv.xy * 7.0, t * 0.5);
    float2 n = float2(Fbm3(p), Fbm3(p + float3(13.7, 4.1, 0.0))) - 0.5;
    float ripple = sin(i.uv.x * 40.0 + i.uv.y * 12.0 - t * 5.0) * 0.5 + 0.5;
    float2 off = n * px * (1.5 + ripple) + ScreenNormal(i) * px * 0.6;

    // A slight prism split: real refraction through a thin moving film.
    float3 seen;
    seen.r = SceneAtSafe(i, off * 1.08).r;
    seen.g = SceneAtSafe(i, off).g;
    seen.b = SceneAtSafe(i, off * 0.92).b;

    // Outline: the grazing edge, its colour turning with the viewing angle and
    // drifting in time. Thin - most of a gun seen nearly edge-on is "edge".
    float fr = Fresnel(i, 5.0);
    float3 edgeCol = Hue(fr * 0.8 + dot(ViewDir(i), float3(0.3, 0.2, 0.5)) + t * 0.05);
    edgeCol = lerp(float3(0.6, 0.9, 1.0), edgeCol, 0.6);
    float3 col = seen + edgeCol * fr * 0.9 * rimK;

    // Crackle: sparse sparks on the edge, re-rolled a dozen times a second.
    float slot = floor(t * 12.0);
    float spark = Noise3(float3(i.uv.xy * 60.0, slot));
    spark = smoothstep(0.82, 0.95, spark) * smoothstep(0.15, 0.6, fr);
    col += float3(0.6, 0.85, 1.0) * spark * 2.5 * crackK;

    // The body: the faintest tint, so a still weapon is not completely gone.
    col = lerp(col, col * 0.8 + edgeCol * 0.2, body);
    return float4(col, 1.0);
}

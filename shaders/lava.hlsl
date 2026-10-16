// An aquarium of lava: the weapon is a clear glass shell, and inside it one
// continuous body of molten lava slowly churns on its own, with bubbles
// rising through it. Seen edge-on the shell is all glass and shows the world
// behind, bent; facing you, you look through the glass into the lava.
//
// The lava lives in the camera's space, not on the weapon's UV: UV is cut into
// islands, and a pattern laid on it falls apart into patches at every cut.
// The viewmodel is fixed to the camera, so the lava stays with the gun - and
// when the gun sways it lags a little, which reads as liquid in a vessel.
//
//   tex0       optional mask by UV, red channel: white = lava, black = glass only
//   param0.x   lava blob size in world units (default 2.2)
//   param0.y   flow speed (default 1)
//   param0.z   how deep the lava sits under the glass, world units (default 0.5)
//   param0.w   glass rim: how far into the faces the clear edge reaches, 0..1 (default 0.5)
//   param1.xyz lava colour (default orange)
//   param1.w   glow (default 1)
//   param2.x   refraction of the glass in pixels (default 25)
//   param2.y   bubbles, 0..1 (default 0.5)
#include "weaponfx.hlsli"
#include "effects.hlsli"

// The camera's axes; forward completes them. Same rows the screen-space
// helpers use, so if those go wrong after an update, this does too.
float3 ToView(float3 v) {
    float3 r = CameraRight(), u = CameraUp();
    float3 f = normalize(cross(r, u));
    return float3(dot(v, r), dot(v, u), dot(v, f));
}

// Small spheres rising in view space. x = inside a bubble, y = its lit rim.
float2 Bubbles(float3 p, float t) {
    p.y -= t * 0.35;                       // up the screen
    float3 c = floor(p), f = frac(p);
    float2 best = 0.0;
    [unroll] for (int z = -1; z <= 1; ++z)
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x) {
        float3 g = float3(x, y, z);
        float3 h = Hash33(c + g);
        if (h.z > 0.22) continue;          // most cells are empty: bubbles stay sparse
        float3 at = g + 0.2 + 0.6 * h;
        at.x += 0.1 * sin(t * 1.7 + h.y * 6.28);
        float r = 0.10 + 0.12 * h.x;
        float d = length(at - f);
        float inside = 1.0 - smoothstep(r * 0.8, r, d);
        float rim = smoothstep(r * 0.55, r * 0.95, d) * inside;
        best = max(best, float2(inside, rim));
    }
    return best;
}

// The environment probes are HDR: the sky in them is many times brighter
// than 1, and added as it is it washes the weapon out white. Squashed to 0..1.
float3 Reflection(float3 V, float3 N) {
    float3 e = Environment(reflect(V, N), 0.0, 1.0);
    return e / (1.0 + e);
}

// Dark red -> orange -> hot yellow, by heat 0..1.
float3 LavaColour(float heat, float3 base) {
    float3 dark = base * float3(0.32, 0.07, 0.04);
    float3 hot  = float3(1.0, 0.62, 0.22);
    float3 c = lerp(dark, base * 0.8, smoothstep(0.2, 0.55, heat));
    return lerp(c, hot, smoothstep(0.78, 1.0, heat));
}

float4 main(PSIn i) : SV_Target {
    float size   = g_fxParam[0].x > 0.0 ? g_fxParam[0].x : 2.2;
    float speed  = g_fxParam[0].y > 0.0 ? g_fxParam[0].y : 1.0;
    float depth  = g_fxParam[0].z > 0.0 ? g_fxParam[0].z : 0.5;
    float rimW   = g_fxParam[0].w > 0.0 ? saturate(g_fxParam[0].w) : 0.5;
    float3 base  = dot(g_fxParam[1].xyz, 1.0) > 0.0 ? g_fxParam[1].xyz : float3(1.0, 0.40, 0.06);
    float glowK  = g_fxParam[1].w > 0.0 ? g_fxParam[1].w : 1.0;
    float refrPx = g_fxParam[2].x > 0.0 ? g_fxParam[2].x : 25.0;
    float bubK   = g_fxParam[2].y > 0.0 ? saturate(g_fxParam[2].y) : 0.5;
    float t = Time() * speed;

    float3 N = Normal(i);
    float3 V = ViewDir(i);                       // eye -> pixel
    float facing = saturate(dot(N, -V));          // 1 = facing the eye, 0 = edge-on

    // Into the glass: the point of the lava seen through this pixel sits
    // `depth` further along the (slightly refracted) view ray.
    float3 viewPos = ToView(i.pos);
    float3 ray = ToView(normalize(V - N * 0.35));
    float3 p = (viewPos + ray * depth) / size;

    // One slow body: the space is warped by large noise, and the heat field
    // drifts upward through it, so the blobs stretch, merge and rise.
    float3 warp = float3(Fbm3(p * 0.5 + float3(0, 0, t * 0.06)),
                         Fbm3(p * 0.5 + float3(13.1, 0, t * 0.06)),
                         Fbm3(p * 0.5 + float3(0, 27.3, t * 0.06))) - 0.5;
    float3 q = p + warp * 1.6 + float3(0, -t * 0.08, 0);
    float heat = Fbm3(q * 1.3);
    heat = saturate((heat - 0.5) * 3.0 + 0.5);

    float2 bub = Bubbles(p * 3.0, t) * bubK;
    heat = lerp(heat, heat * 0.6, bub.x);
    float3 lava = LavaColour(heat, base) * (0.45 + 0.35 * glowK);   // the frame is HDR: above ~1 it washes out pale
    lava += bub.y * float3(1.0, 0.7, 0.4) * 0.5;

    // Where the lava is: everywhere but a clear glass rim toward the edges.
    float mask = HasTex0() ? Tex0(i).r : 1.0;
    float rim0 = rimW * 0.5, rim1 = rimW * 0.5 + 0.25;
    float inside = smoothstep(rim0, rim1, facing) * mask;
    // The lava's own edge against the glass glows, as in a lit vessel.
    float lip = exp(-pow((facing - lerp(rim0, rim1, 0.5)) / 0.07, 2.0)) * mask;

    // The shell: the world behind, bent, warmed by the lava it holds.
    float fr = Fresnel(i, 5.0);
    float3 refl = Reflection(V, N);
    float2 off = ScreenNormal(i) * refrPx;
    float3 seen = float3(SceneAt(i, off * 1.08).r, SceneAt(i, off).g, SceneAt(i, off * 0.92).b);
    float3 glass = seen * float3(0.93, 0.96, 1.0) * (1.0 - fr * 0.5) + refl * fr * 0.45;
    glass += base * (0.05 + 0.2 * lip) * glowK * mask;

    float3 c = lerp(glass, lava, inside);
    c += base * lip * 0.3 * glowK;

    // The glass skin over the lava too: reflection by Fresnel and a hard
    // highlight, so the lava reads as behind glass and not painted on.
    float spec = pow(saturate(dot(reflect(V, N), normalize(float3(0.3, 0.2, 1.0)))), 80.0);
    c += inside * (refl * (0.04 + fr * 0.3) + spec * 0.8);
    return float4(c, 1.0);
}

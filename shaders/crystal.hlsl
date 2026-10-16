// Cut crystal: the weapon is a clear gem cut into facets. Each facet leans its
// own way, so it shows its own piece of the world behind, split into colours,
// and catches the light in a glint that flashes as the weapon turns. A second
// layer of facets deeper inside, seen with parallax, gives the stone depth.
//   param0.x  facets across the UV (default 14)
//   param0.y  refraction in pixels (default 35)
//   param0.z  dispersion, fraction of the offset (default 0.25)
//   param0.w  glint strength (default 1; negative = none)
//   param1.xyz tint of the stone (default ice white)
#include "weaponfx.hlsli"

float2 Hash22(float2 p) {
    float3 q = frac(float3(p.xyx) * float3(0.1031, 0.1030, 0.0973));
    q += dot(q, q.yzx + 33.33);
    return frac((q.xx + q.yz) * q.zy);
}

// Voronoi in 2D: the nearest cell's id, and how far the pixel is from the
// border with the next one (0 on a facet edge).
void Facets(float2 p, out float2 id, out float edge) {
    float2 c = floor(p), f = frac(p);
    float d1 = 8.0, d2 = 8.0;
    float2 best = c;
    float2 bestAt = 0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x) {
        float2 g = float2(x, y);
        float2 at = g + Hash22(c + g) * 0.85 + 0.075;
        float d = dot(at - f, at - f);
        if (d < d1) { d2 = d1; d1 = d; best = c + g; bestAt = at; }
        else if (d < d2) { d2 = d; }
    }
    id = best;
    edge = sqrt(d2) - sqrt(d1);
}

// A facet's normal: the surface normal leaned by a random amount per cell.
float3 FacetNormal(float3 n, float2 id, float lean) {
    float3 r = Hash33(float3(id, 7.0)) * 2.0 - 1.0;
    return normalize(n + r * lean);
}

float2 OnScreen(float3 n) { return float2(dot(n, CameraRight()), -dot(n, CameraUp())); }

float4 main(PSIn i) : SV_Target {
    float scale = g_fxParam[0].x > 0.0 ? g_fxParam[0].x : 14.0;
    float px    = g_fxParam[0].y > 0.0 ? g_fxParam[0].y : 35.0;
    float disp  = g_fxParam[0].z > 0.0 ? g_fxParam[0].z : 0.25;
    float glntK = g_fxParam[0].w == 0.0 ? 1.0 : max(g_fxParam[0].w, 0.0);
    float3 tint = dot(g_fxParam[1].xyz, 1.0) > 0.0 ? g_fxParam[1].xyz : float3(0.9, 0.96, 1.0);

    float3 n = Normal(i);
    float3 v = ViewDir(i);

    // The outer cut.
    float2 id; float edge;
    Facets(i.uv.xy * scale, id, edge);
    float3 fn = FacetNormal(n, id, 0.9);
    float2 off = OnScreen(fn) * px;

    // The inner cut: a second facet grid, shifted by the view so it slides
    // under the outer one as the weapon moves - the stone looks deep.
    float2 par = float2(dot(v, CameraRight()), dot(v, CameraUp())) * 0.35;
    float2 id2; float edge2;
    Facets(i.uv.xy * scale * 0.6 + par * scale + 3.7, id2, edge2);
    off += OnScreen(FacetNormal(n, id2, 0.6)) * px * 0.5;

    // Five taps across the spectrum: a wide, clean rainbow at every facet.
    float3 seen = 0;
    float3 w = 0;
    [unroll] for (int k = 0; k < 5; ++k) {
        float s = k / 4.0;                       // 0 red .. 1 violet
        float3 band = saturate(float3(1.5 - abs(s - 0.0) * 3.0, 1.5 - abs(s - 0.45) * 3.0,
                                      1.5 - abs(s - 1.0) * 3.0));
        seen += SceneAtSafe(i, off * (1.0 + disp * (0.5 - s) * 2.0)) * band;
        w += band;
    }
    seen /= max(w, 1e-3);
    float3 col = seen * tint;

    // Facet edges: thin, bright lines where two faces meet, faint inner ones.
    col += smoothstep(0.06, 0.0, edge) * 0.35 * float3(0.9, 0.95, 1.0);
    col += smoothstep(0.05, 0.0, edge2) * 0.12;

    // Reflection off the facet and a glint: each facet flashes when its
    // mirror direction lines up with a light that slowly circles above.
    float3 r = reflect(v, fn);
    float fr = pow(1.0 - saturate(dot(fn, -v)), 4.0);
    col = lerp(col, Environment(r, 0, 0) * tint, fr * 0.5);
    float a = Time() * 0.4;
    float3 light = normalize(float3(cos(a), sin(a), 1.6));
    float g = pow(saturate(dot(r, light)), 300.0) * step(0.6, Hash13(float3(id, 3.0)));
    col += g * 6.0 * glntK;
    return float4(col, 1.0);
}

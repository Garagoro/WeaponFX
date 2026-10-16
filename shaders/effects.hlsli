// The effects as functions, so one shader can mix several (masked.hlsl). Each
// takes its knobs as arguments; the per-effect .hlsl files map g_fxParam onto
// them. Include after weaponfx.hlsli.

// --- galaxy: a window into space ------------------------------------------

float3 StarLayer(float3 dir, float scale, float density) {
    float3 p = dir * scale;
    float3 cell = floor(p);
    float3 f = frac(p);
    float3 at = Hash33(cell) * 0.8 + 0.1;
    float lit = step(1.0 - density, Hash13(cell + 7.7));
    float d = length(f - at);
    float twinkle = 0.75 + 0.25 * sin(Time() * (2.0 + 5.0 * Hash13(cell)) + Hash13(cell) * 40.0);
    float3 tint = lerp(float3(0.7, 0.8, 1.0), float3(1.0, 0.85, 0.6), Hash13(cell + 3.1));
    return tint * lit * twinkle * smoothstep(0.16, 0.0, d);
}

// Looked up by the world-space view ray, so turning the camera moves the sky
// across the weapon; the near star layers are offset by world position too, so
// walking gives them parallax against the far ones. The weapon covers ~15
// degrees of view, so features are sized for that.
float3 Galaxy(PSIn i, float nebulaK, float rimK) {
    float3 d = ViewDir(i);
    float3 w = WorldPos(i);
    float3 neb = d * 7.0 + float3(0.0, 0.0, Time() * 0.03);
    float n1 = Fbm3(neb);
    float n2 = Fbm3(neb * 1.7 + 5.0);
    float3 col = float3(0.01, 0.005, 0.03);
    col += nebulaK * (float3(0.55, 0.12, 0.70) * pow(smoothstep(0.45, 0.85, n1), 2.0) * 0.8 +
                      float3(0.05, 0.30, 0.65) * pow(smoothstep(0.50, 0.90, n2), 2.0) * 0.7);
    col += StarLayer(d, 260.0, 0.18) * 1.5;
    col += StarLayer(normalize(d + w * 0.0004), 130.0, 0.10) * 2.5;
    col += StarLayer(normalize(d + w * 0.0010), 60.0, 0.05) * 3.5;
    // Weak: the gun is seen nearly edge-on, a strong rim covers all of it.
    col += rimK * pow(Fresnel(i, 1.0), 6.0) * float3(0.35, 0.55, 1.0) * 0.5;
    return col;
}

// --- chrome: polished metal ------------------------------------------------

float3 Chrome(PSIn i, float slice, float mip, float3 tint) {
    float3 r = reflect(ViewDir(i), Normal(i));
    float3 col = Environment(r, slice, mip) * tint;
    float sweep = frac(i.uv.x * 0.5 - Time() * 0.25);
    col += smoothstep(0.03, 0.0, abs(sweep - 0.5)) * 0.6;
    return col;
}

// --- plasma: molten energy along the UV -------------------------------------

float3 Plasma(PSIn i, float speed, float scale) {
    float t = Time() * speed;
    float3 p = float3(i.uv.xy * 6.0 * scale, t * 0.3);
    float2 warp = float2(Fbm3(p + float3(0, 0, 0)), Fbm3(p + float3(5.2, 1.3, 0)));
    float v = Fbm3(p + float3(warp * 3.0 - float2(t * 0.6, 0.0), 0.0));
    float vein = pow(saturate(1.0 - abs(v - 0.5) * 6.0), 4.0);
    float pulse = 0.8 + 0.2 * sin(t * 3.0);
    float3 col = lerp(float3(0.05, 0.0, 0.08), float3(0.9, 0.15, 0.05), smoothstep(0.3, 0.7, v));
    col += vein * float3(1.0, 0.7, 0.2) * 2.0 * pulse;
    col += Fresnel(i, 4.0) * float3(1.0, 0.4, 0.1) * 0.6;
    return col;
}

// --- hologram ---------------------------------------------------------------

float3 Hologram(PSIn i, float3 tint) {
    float t = Time();
    float3 w = WorldPos(i);
    float band = smoothstep(0.85, 1.0, frac(w.z * 0.6 - t * 0.8));
    float lines = 0.75 + 0.25 * sin(i.svpos.y * 1.6);
    float glitchSlot = floor(t * 12.0);
    float glitch = step(0.93, Hash13(float3(glitchSlot, 0.0, 0.0)));
    float rim = Fresnel(i, 2.0);
    float3 col = tint * (0.08 + rim * 1.2 + band * 0.8) * lines;
    col *= 1.0 - glitch * 0.6 * step(0.5, Hash13(float3(floor(i.svpos.y / 8.0), glitchSlot, 1.0)));
    return col;
}

// --- glass: the scene behind, bent by the surface ---------------------------

// `strengthPx` is how far, in pixels, a surface facing fully sideways pushes
// the image; `dispersion` splits the channels by that fraction for a prism
// fringe; `tint` multiplies what is seen through. A thin reflection of the
// environment rides on top by Fresnel, as on real glass.
float3 Glass(PSIn i, float strengthPx, float dispersion, float3 tint, float slice) {
    float2 off = ScreenNormal(i) * strengthPx;
    float3 seen;
    seen.r = SceneAt(i, off * (1.0 + dispersion)).r;
    seen.g = SceneAt(i, off).g;
    seen.b = SceneAt(i, off * (1.0 - dispersion)).b;
    float fr = Fresnel(i, 5.0);
    float3 refl = Environment(reflect(ViewDir(i), Normal(i)), slice, 0.0);
    return lerp(seen * tint, refl, fr * 0.6) + fr * 0.08;
}

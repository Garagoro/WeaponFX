// WeaponFX common header: what the engine hands our pixel shader on the weapon
// in hands, and what WeaponFX adds. Include it first in every effect:
//     #include "weaponfx.hlsli"
//     float4 main(PSIn i) : SV_Target { ... }
//
// PSIn is the input signature of the marker material's own pixel shader
// (csgo_unlitgeneric, F_TWOTEXTURE), captured 2026-10-15. Same semantics, same
// order, same widths, so it packs into the registers the engine's vertex shader
// writes. Reordering or dropping a field shifts every register after it and
// the weapon reads garbage with no error.
struct PSIn {
    float3 pos : TEXCOORD0;               // position relative to the camera - use WorldPos()/ViewDir()
    float3 nrm : TEXCOORD1;               // normal, world space
    float4 uv  : TEXCOORD2;               // xy, zw: the two texture sets
    centroid float4 col : TEXCOORD4;      // vertex colour
    nointerpolation uint id : TEXCOORD12;
    float4 svpos : SV_Position;           // pixel centre, in pixels
    bool front : SV_IsFrontFace;
};

// The engine's buffers, still bound because WeaponFX swaps only the shader.
// Indices are from that one captured shader; a game update can move them, and
// the symptom is a frozen or black effect while UV-only effects keep working.
// Dumped at the weapon draw (RENDERING.md, 5): b1[26] and b3[35] both hold the
// camera position, b1[28].xyz the camera forward, b1[21].zw the screen size.
cbuffer EngineView  : register(b1) { float4 g_view[40]; };   // [26].xyz camera position
cbuffer EngineLayer : register(b3) { float4 g_l3[64]; };     // [35].xyz camera position (the pos offset)
TextureCubeArray g_envCubes : register(t10);   // environment probes (BC6H, HDR)
SamplerState     g_linear   : register(s0);

// WeaponFX's own buffer (weapon_fx.cpp, FxConstants).
cbuffer WeaponFx : register(b13) {
    float4 g_fxTime;       // x seconds since load, y delta to previous draw, z draw counter
    float4 g_fxScreen;     // xy viewport size in pixels, zw 1/size
    float4 g_fxParam[4];   // param0..param3 in weaponfx.ini
};

// WeaponFX's textures (weapon_fx.h kSceneSlot). g_fxScene is the frame as it
// stood before the weapon was drawn - copied only for a shader that reads it.
// g_fxTex0..3 are image files: tex0..tex3 in weaponfx.ini; unset ones read 0.
Texture2D g_fxScene : register(t32);
Texture2D g_fxTex0  : register(t33);
Texture2D g_fxTex1  : register(t34);
Texture2D g_fxTex2  : register(t35);
Texture2D g_fxTex3  : register(t36);
// The scene's depth at the same moment as g_fxScene (before the viewmodel).
// Raw depth-buffer values; SceneDistance() turns them into world units.
Texture2D<float> g_fxDepth : register(t37);
// Last frame's g_fxScene. SceneAt() falls back to it where this frame's copy is
// pure black: the copy taken at a wrong moment (the world not drawn yet under
// the weapon) shows as a frame-old background instead of a black blink.
Texture2D g_fxScenePrev : register(t38);

float  Time()            { return g_fxTime.x; }
float3 WorldPos(PSIn i)  { return i.pos + g_l3[35].xyz; }
float3 CameraPos()       { return g_view[26].xyz; }
// From the eye to this pixel, world space, normalised.
float3 ViewDir(PSIn i)   { return normalize(WorldPos(i) - CameraPos()); }
float3 Normal(PSIn i)    { return normalize(i.nrm); }
float2 ScreenUV(PSIn i)  { return i.svpos.xy * g_fxScreen.zw; }

// Reflection of the world around the weapon. Which probe slice belongs to the
// player's spot is not known yet; 0 is the first one bound.
float3 Environment(float3 dir, float slice, float mip) {
    return g_envCubes.SampleLevel(g_linear, float4(dir, slice), mip).rgb;
}

float Fresnel(PSIn i, float power) {
    return pow(1.0 - saturate(dot(Normal(i), -ViewDir(i))), power);
}

// Camera axes in world space, read off the view rotation in b1[8..10] (the
// columns' x and y). Same dump as the camera position; if the screen-space
// offsets below move the wrong way after an update, these rows moved.
float3 CameraRight() { return normalize(float3(g_view[8].x, g_view[9].x, g_view[10].x)); }
float3 CameraUp()    { return normalize(float3(g_view[8].y, g_view[9].y, g_view[10].y)); }

// The normal turned into a screen-space direction: +x right, +y down.
float2 ScreenNormal(PSIn i) {
    float3 n = Normal(i);
    return float2(dot(n, CameraRight()), -dot(n, CameraUp()));
}

// The scene behind this pixel, displaced by `offsetPx` pixels.
float3 SceneAt(PSIn i, float2 offsetPx) {
    float w, h;
    g_fxScene.GetDimensions(w, h);
    float2 uv = saturate((i.svpos.xy + offsetPx) / float2(max(w, 1.0), max(h, 1.0)));
    float3 c = g_fxScene.SampleLevel(g_linear, uv, 0.0).rgb;
    // Pure black is never a lit world in HDR; an unbound g_fxScenePrev reads
    // 0 and changes nothing.
    if (dot(c, c) < 1e-8) c = g_fxScenePrev.SampleLevel(g_linear, uv, 0.0).rgb;
    return c;
}

// SceneAt for big shifts: a read that lands on pure black (behind a hand drawn
// after the copy, where the world is never drawn) steps back towards this
// pixel - half, a quarter, then unshifted - instead of painting black. Every
// effect that bends the world by more than a few pixels wants this one.
float3 SceneAtSafe(PSIn i, float2 offsetPx) {
    float3 c = SceneAt(i, offsetPx);
    [unroll] for (int k = 0; k < 3; ++k) {
        if (dot(c, c) >= 1e-6) break;
        c = SceneAt(i, offsetPx * (k == 2 ? 0.0 : (k == 0 ? 0.5 : 0.25)));
    }
    return c;
}

// --- depth ------------------------------------------------------------------
// The projection is b1[12..15], rows (captured 2026-10-15): [13].y the
// vertical focal scale (1/tan(fov/2)), [14].zw the depth terms. It is a plain
// (not reversed) depth with near ~6.6: d = A - B / dist, A = -[14].z,
// B = -[14].w. If a game update flips to reversed Z, distances come out
// negative or huge and the depth-aware bend goes flat or wild.
bool  HasDepth()       { float w, h; g_fxDepth.GetDimensions(w, h); return w > 0.0; }
// Distance along the view axis to what the scene holds at this pixel.
float SceneDistance(float2 px) {
    float w, h;
    g_fxDepth.GetDimensions(w, h);
    float d = g_fxDepth.Load(int3(clamp(px, float2(0, 0), float2(w, h) - 1.0), 0));
    float A = -g_view[14].z, B = -g_view[14].w;
    return B / max(A - d, 1e-6);
}
// Pixels per radian at the screen centre: how far an angle moves things.
float FocalPx()        { return 0.5 * g_fxScreen.y * g_view[13].y; }
// This pixel's distance from the eye (the weapon is a few units away).
float PixelDistance(PSIn i) { return length(i.pos); }
bool  HasTex0()        { float w, h; g_fxTex0.GetDimensions(w, h); return w > 0.0; }

// A mask texture by the weapon's first UV set. Texture index is a literal in
// the caller (HLSL cannot index separate Texture2D objects).
float4 Tex0(PSIn i) { return g_fxTex0.Sample(g_linear, i.uv.xy); }
float4 Tex1(PSIn i) { return g_fxTex1.Sample(g_linear, i.uv.xy); }

// Hashes and noise: integer-free so they compile the same on every driver.
float  Hash13(float3 p) { p = frac(p * 0.1031); p += dot(p, p.zyx + 31.32); return frac((p.x + p.y) * p.z); }
float3 Hash33(float3 p) {
    p = frac(p * float3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return frac((p.xxy + p.yxx) * p.zyx);
}
float Noise3(float3 p) {
    float3 c = floor(p), f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(lerp(Hash13(c + float3(0, 0, 0)), Hash13(c + float3(1, 0, 0)), f.x),
                     lerp(Hash13(c + float3(0, 1, 0)), Hash13(c + float3(1, 1, 0)), f.x), f.y),
                lerp(lerp(Hash13(c + float3(0, 0, 1)), Hash13(c + float3(1, 0, 1)), f.x),
                     lerp(Hash13(c + float3(0, 1, 1)), Hash13(c + float3(1, 1, 1)), f.x), f.y), f.z);
}
float Fbm3(float3 p) {
    float v = 0.0, a = 0.5;
    [unroll] for (int k = 0; k < 5; ++k) { v += a * Noise3(p); p = p * 2.03 + 17.1; a *= 0.5; }
    return v;
}

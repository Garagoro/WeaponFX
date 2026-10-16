// Diagnostics for the spaces WeaponFX relies on. param0.x picks the view:
//   0  frac(world position / 16): bands every 16 units, should slide over the gun as you walk
//   1  frac(|pos| / 16) of the raw interpolant (camera-relative if it bands around the eye)
//   2  normalize(raw pos) as colour
//   3  distance eye -> pixel, frac(/16)
//   4  dot(normal, -view) as grey: bright where the surface faces the camera
#include "weaponfx.hlsli"

float4 main(PSIn i) : SV_Target {
    int m = (int)g_fxParam[0].x;
    if (m == 1) return float4(frac(length(i.pos) / 16.0).xxx, 1.0);
    if (m == 2) return float4(normalize(i.pos) * 0.5 + 0.5, 1.0);
    if (m == 3) return float4(frac(length(WorldPos(i) - CameraPos()) / 16.0).xxx, 1.0);
    if (m == 4) return float4(saturate(dot(Normal(i), -ViewDir(i))).xxx, 1.0);
    return float4(frac(WorldPos(i) / 16.0), 1.0);
}

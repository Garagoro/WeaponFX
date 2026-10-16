#include "weaponfx.hlsli"
// Copy straight through, red where the run copy is exactly black.
float4 main(PSIn i) : SV_Target {
    float3 c = SceneAt(i, float2(0, 0));
    if (dot(c, c) < 1e-6) return float4(1, 0, 0, 1);
    return float4(c, 1);
}

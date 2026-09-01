#ifndef TERRAIN_BRUSH_PARAMS_GLSL
#define TERRAIN_BRUSH_PARAMS_GLSL

// Shared brush parameter block for the GPU terrain authoring path (issue #716).
//
// ONE block for BOTH brush kernels (Terrain_SculptBrush.comp and
// Terrain_PaintBrush.comp) on purpose: binding 83 is the LAST uniform-buffer
// index below the GL 4.6 minimum guarantee of 84 (ShaderBindingLayout's
// MIN_GUARANTEED_BUFFER_BINDINGS), so a second block for the paint kernel would
// have had nowhere to live. The two kernels use overlapping subsets of the
// fields — Src*/TargetHeight/InvHeightScale are sculpt-only, TargetLayer /
// LayerCount are paint-only — and it is refilled immediately before each
// dispatch per the issue #691 pattern.
//
// C++ twin: UBOStructures::TerrainBrushUBO (ShaderBindingLayout::UBO_TERRAIN_BRUSH).
layout(std140, binding = 83) uniform TerrainBrushParams
{
    ivec4 u_Rect;       //  0 — destination rect (x, y, w, h) in texels
    ivec4 u_SrcRect;    // 16 — sculpt scratch region (x, y, w, h) in texels
    vec2  u_CenterNorm; // 32 — brush centre in normalized terrain coords [0,1]
    vec2  u_WorldSize;  // 40 — terrain world size (X, Z)
    float u_Radius;     // 48 — brush radius in WORLD units
    float u_StrengthDt; // 52 — Strength * deltaTime (already multiplied CPU-side)
    float u_Falloff;    // 56 — 0 = hard, 1 = fully smooth
    float u_TargetHeight;   // 60 — sculpt Flatten/Level target, normalized height
    float u_InvHeightScale; // 64 — 1 / heightScale, so Raise/Lower move world units
    int   u_Tool;           // 68 — TerrainBrushTool: 0 Raise, 1 Lower, 2 Smooth, 3 Flatten, 4 Level
    int   u_Resolution;     // 72 — square texture resolution of the edited target
    int   u_TargetLayer;    // 76 — paint: layer index [0, 8)
    int   u_LayerCount;     // 80 — paint: how many layers participate in normalization
    // THREE SEPARATE INTS, not an ivec3. std140 gives ivec3 a 16-byte base
    // alignment, which would push the pad to offset 96 and make the block 112
    // bytes against the 96-byte buffer the C++ twin allocates and uploads. Scalars
    // align to 4, so these land at 84/88/92 and close the block at exactly 96.
    int _terrainBrushPad0; // 84
    int _terrainBrushPad1; // 88
    int _terrainBrushPad2; // 92
};

// Falloff curve. Byte-for-byte the CPU TerrainBrushUtils::ComputeFalloff, which
// is what lets TerrainGPUBrushParityTest compare the two implementations rather
// than merely assert that each is self-consistent.
float terrainBrushFalloff(float dist, float radius, float falloff)
{
    if (radius <= 0.0 || dist >= radius)
        return 0.0;

    falloff = clamp(falloff, 0.0, 1.0);
    float t = dist / radius;
    float smoothW = 0.5 * (1.0 + cos(t * 3.14159265358979323846));
    return mix(1.0, smoothW, falloff);
}

// World-space distance from the brush centre to texel (x, z). The CPU brush
// measures in WORLD units, not texels — a circular brush on a non-square terrain
// is an ellipse in texel space, and computing it in the wrong space is exactly
// the failure docs/agent-rules/cpu-gpu-surface-parity.md is about.
float terrainBrushDistance(ivec2 texel)
{
    float inv = 1.0 / float(u_Resolution - 1);
    float dx = (float(texel.x) * inv - u_CenterNorm.x) * u_WorldSize.x;
    float dz = (float(texel.y) * inv - u_CenterNorm.y) * u_WorldSize.y;
    return sqrt(dx * dx + dz * dz);
}

#endif // TERRAIN_BRUSH_PARAMS_GLSL

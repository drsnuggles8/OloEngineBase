// =============================================================================
// MaterialLabProbe.glsl
//
// Material-laboratory probe for issue #975 (PBR closure v2). Serves the
// acceptance criterion "The material laboratory benchmark captures v2
// roughness/metallicity sweeps from multiple view angles": it shades a 7x7
// grid of analytic spheres — x sweeps authored roughness 0..1, y sweeps
// metallic 0..1 — through the REAL versioned dispatch `evaluatePBRClosure`
// (PBRCommon.glsl), with the model (Legacy / ClosureV2) and the view
// direction supplied per draw via a small params UBO. The C++ harness
// (MaterialLabVisualEvidenceTest.cpp) renders both models from three view
// angles and writes the six frames as PNG evidence.
//
// Deliberately NOT in scope: issue #974's benchmark-scene manifest/tooling.
// This probe is self-contained (fullscreen analytic shading, no scene, no
// manifest) precisely so that seam stays clean.
//
// Layout (image is square, 7x7 equal cells):
//   - column i (v_TexCoord.x): roughness = i / 6  (0, 1/6, ..., 1)
//   - row    j (v_TexCoord.y): metallic  = j / 6  (0, 1/6, ..., 1)
//   Each cell ray-free reconstructs a front hemisphere from the cell-local
//   disk (orthographic normal reconstruction); outside the disk it outputs a
//   flat dark BACKDROP of vec3(0.08) linear (~78 after tonemap+gamma).
//   NOTE FOR TEST AUTHORS: the backdrop is deliberately NOT black — the
//   test's contracts must not assume absolute darkness anywhere.
//
// Roughness is passed to evaluatePBRClosure UN-clamped, exactly as
// calculateLightContribution passes the material's authored roughness. That
// makes the Legacy near-mirror collapse (distributionGGX's a2 = 0 numerator
// at authored roughness 0) and ClosureV2's alpha-floor near-mirror fix both
// visible in the same capture — which is the point of the laboratory.
//
// Params UBO binding choice: the sphere-area probe (the closest visual-probe
// precedent) is fully constant-driven and declares NO params UBO, so the
// convention followed here is the ShaderUnit test-probe one: binding 18,
// the slot ShaderUnit_ShadowSelfShadow / ShaderUnit_ShadowVisualize use for
// their ShadowProbeUBO. Test shaders are excluded from the production
// binding scan (ShaderHarness skips tests/), this shader declares no other
// resource at 18 (the within-shader uniqueness rule), and the production
// occupant of slot 18 (UBO_PRECIPITATION) never runs during a probe draw.
// =============================================================================

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;

layout(location = 0) out vec2 v_TexCoord;

void main()
{
    v_TexCoord = a_TexCoord;
    gl_Position = vec4(a_Position, 1.0);
}

#type fragment
#version 460 core

layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec2 v_TexCoord;

#include "include/PBRCommon.glsl"

// Per-draw parameters — mirrored byte-for-byte by MaterialLabParamsUbo in
// MaterialLabVisualEvidenceTest.cpp (std140: two 16-byte members, 32 bytes).
layout(std140, binding = 18) uniform MaterialLabParams
{
    vec4  u_ViewDir;     // xyz = normalized view direction, w unused
    ivec4 u_ProbeParams; // x = pbr model (OLO_PBR_MODEL_LEGACY=0 /
                         //     OLO_PBR_MODEL_CLOSURE_V2=1), yzw unused
};

// Grid layout — must match the constants in MaterialLabVisualEvidenceTest.cpp.
const int kGridSize = 7;

// Disk radius in cell-local NDC ([-1,1] per cell). 0.85 leaves a visible
// backdrop margin between spheres so cells read as separate samples.
const float kDiskRadius = 0.85;

// Fixed mid-grey-warm albedo. For metallic rows this doubles as F0, so the
// energy-compensation contrast (which scales with F0) is strong on the
// metallic edge of the grid and near-absent on the dielectric edge — the
// pairing the test's anti-vacuous contract relies on.
const vec3 kAlbedo = vec3(0.85, 0.83, 0.8);

// TWO directional lights: a warm key that puts a clear specular peak on the
// upper-right of each sphere, and a dim cool fill so the shadowed side keeps
// some shape. Directions are constants — only the VIEW direction and the
// model vary per draw (via the UBO), keeping the six captures comparable.
const vec3 kKeyDir    = normalize(vec3(0.45, 0.55, 0.70));
const vec3 kKeyColor  = vec3(1.0, 0.98, 0.95) * 2.2;
const vec3 kFillDir   = normalize(vec3(-0.60, -0.35, 0.55));
const vec3 kFillColor = vec3(0.55, 0.65, 0.80) * 0.35;

// Flat backdrop outside each sphere's disk. Deliberately NOT black (see
// header note): after Reinhard + gamma it lands near grey level 78.
const vec3 kBackdrop = vec3(0.08);

// Small constant ambient so the dark limb of each sphere never hits pure
// black — the sweep stays legible in the PNG and differential contracts
// never divide by a zero-signal region.
const vec3 kAmbient = 0.03 * kAlbedo;

void main()
{
    // Map v_TexCoord into (column, row, cell-local NDC).
    float colF = clamp(v_TexCoord.x, 0.0, 1.0) * float(kGridSize);
    float rowF = clamp(v_TexCoord.y, 0.0, 1.0) * float(kGridSize);
    int col = clamp(int(floor(colF)), 0, kGridSize - 1);
    int row = clamp(int(floor(rowF)), 0, kGridSize - 1);
    vec2 ndc = fract(vec2(colF, rowF)) * 2.0 - 1.0;

    // Authored material parameters for this cell (endpoints included).
    float roughness = float(col) / float(kGridSize - 1);
    float metallic  = float(row) / float(kGridSize - 1);

    // Analytic sphere: inside the disk, reconstruct the front-hemisphere
    // normal orthographically; outside, flat backdrop.
    vec2 p = ndc / kDiskRadius;
    float r2 = dot(p, p);
    if (r2 > 1.0)
    {
        o_Color = vec4(linearToSRGB(reinhardToneMapping(kBackdrop)), 1.0);
        return;
    }
    vec3 N = normalize(vec3(p.x, p.y, sqrt(max(1.0 - r2, 0.0))));

    // View direction from the params UBO — the same reconstructed hemisphere
    // is shaded under head-on / oblique / grazing V, which sweeps NdotV,
    // Fresnel and the highlight position without touching the grid mapping.
    vec3 V = normalize(u_ViewDir.xyz);

    // Direct lighting through the versioned closure dispatch — the exact
    // seam calculateLightContribution routes punctual lights through.
    vec3 Lo = vec3(0.0);

    float NdotLKey = max(dot(N, kKeyDir), 0.0);
    if (NdotLKey > 0.0)
    {
        Lo += evaluatePBRClosure(u_ProbeParams.x, N, V, kKeyDir,
                                 kAlbedo, metallic, roughness) *
              kKeyColor * NdotLKey;
    }

    float NdotLFill = max(dot(N, kFillDir), 0.0);
    if (NdotLFill > 0.0)
    {
        Lo += evaluatePBRClosure(u_ProbeParams.x, N, V, kFillDir,
                                 kAlbedo, metallic, roughness) *
              kFillColor * NdotLFill;
    }

    // Reinhard + gamma (PBRCommon's own helpers) so the HDR specular peaks
    // stay legible in an RGBA8 PNG while relative brightness ordering — the
    // thing the differential contracts measure — is preserved.
    vec3 colorLinear = kAmbient + Lo;
    o_Color = vec4(linearToSRGB(reinhardToneMapping(colorLinear)), 1.0);
}

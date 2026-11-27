// =============================================================================
// MaterialLabProbe.glsl
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
// MaterialLabVisualEvidenceTest.cpp (std140: one 16-byte vec4).
layout(std140, binding = 18) uniform MaterialLabParams
{
    vec4 u_ViewDir; // xyz = view direction, w = 1 to omit multiple scattering for the control
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
// compensation control vary per draw.
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

vec3 evaluateLabLight(vec3 N, vec3 V, vec3 L, float metallic, float roughness)
{
    if (u_ViewDir.w < 0.5)
        return closureV2Evaluate(N, V, L, kAlbedo, metallic, roughness);

    vec3 F0 = mix(vec3(DEFAULT_DIELECTRIC_F0), kAlbedo, metallic);
    vec3 single = closureV2SingleScatter(N, V, L, F0, closureV2Roughness(roughness));
    ClosureV2Energy energy = closureV2Energy(max(dot(N, V), 0.0), max(dot(N, L), 0.0), roughness, F0);
    return single + energy.DiffuseCoupling * (1.0 - metallic) * kAlbedo * INV_PI;
}

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

    // Direct lighting through the shared closure — the exact
    // seam calculateLightContribution routes punctual lights through.
    vec3 Lo = vec3(0.0);

    float NdotLKey = max(dot(N, kKeyDir), 0.0);
    if (NdotLKey > 0.0)
    {
        Lo += evaluateLabLight(N, V, kKeyDir, metallic, roughness) *
              kKeyColor * NdotLKey;
    }

    float NdotLFill = max(dot(N, kFillDir), 0.0);
    if (NdotLFill > 0.0)
    {
        Lo += evaluateLabLight(N, V, kFillDir, metallic, roughness) *
              kFillColor * NdotLFill;
    }

    // Reinhard + gamma (PBRCommon's own helpers) so the HDR specular peaks
    // stay legible in an RGBA8 PNG while relative brightness ordering — the
    // thing the differential contracts measure — is preserved.
    vec3 colorLinear = kAmbient + Lo;
    o_Color = vec4(linearToSRGB(reinhardToneMapping(colorLinear)), 1.0);
}

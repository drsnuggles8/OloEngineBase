// =============================================================================
// ShaderUnit_ShadowSelfShadow.glsl
//
// Self-shadowing / peter-panning probe for CSM shadow bias. Replicates the
// production `sampleShadowPCF` and the inner PCF+bias logic of
// `calculateCascadedShadowFactorCSM` against a real `sampler2DArrayShadow`
// bound at the production binding (8). Everything *outside* the PCF path —
// cascade selection, frustum short-circuit, distance fade — is already
// covered by `ShaderUnit_ShadowBounds.glsl`; here we force cascade 0 with an
// identity light-space matrix so the test can drive `projCoords.z` directly
// via the fragment's world-space Z and observe exactly where the shadow
// transition falls.
//
// UV encoding (set by the test fixture):
//   v_TexCoord.x → projCoords.z (fragment's light-space normalised depth)
//                  — swept across [0, 1] so the host can find the transition
//   v_TexCoord.y → projCoords.xy  sample position inside the shadow map
//                  (the test authors a uniform or step-function map; Y is
//                  either irrelevant or deliberately spans the step)
//
// Output encoding:
//   R: PCF shadow factor (0 = shadowed, 1 = lit; 3x3 kernel so valid values
//      are {0, 1/9, 2/9, ..., 1})
//   G: projCoords.z (echoed so the test can correlate without recomputing)
//   B: bias applied this fragment (echoed)
//   A: 1
//
// Self-shadow invariant (test assertion): for a flat shadow map at depth D,
// fragments at world-space Z == D must return shadow = 1.0 (no false
// self-shadowing). The PCF compare is `projCoords.z - bias <= storedDepth`,
// so bias shifts the reference one step toward the light; any positive
// bias at all is sufficient.
//
// Peter-panning invariant: the shadow transition for a flat shadow map at
// depth D happens at projCoords.z = D + bias. If bias is too large the
// transition is pushed past the real occluder and surfaces "float". We
// expose `bias` on the B channel so the test can assert the transition
// landed in [D, D + bias + tolerance].
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

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 o_Color;

// Matches production TEX_SHADOW binding.
layout(binding = 8) uniform sampler2DArrayShadow u_ShadowMapCSM;

// Test UBO — driven entirely by the host so we can sweep every knob.
layout(std140, binding = 18) uniform ShadowProbeUBO
{
    // x=bias, y=unused, z=unused, w=unused — only bias matters for this probe.
    vec4 u_ShadowParams;
    // Must match the shadow-map texture resolution set up by the host.
    int  u_ShadowResolution;
    // Reserved padding to satisfy std140 layout (vec4 + int + 3 pads).
    int  u_Pad0;
    int  u_Pad1;
    int  u_Pad2;
};

// Byte-for-byte replica of PBRCommon.glsl::sampleShadowPCF so the test
// exercises the exact production kernel. Any divergence from production
// math should be caught by manual diff (or by a golden-image test later).
float sampleShadowPCF(sampler2DArrayShadow shadowMap, vec3 projCoords,
                      float layer, float bias, int resolution)
{
    float shadow = 0.0;
    float texelSize = 1.0 / float(resolution);

    for (int x = -1; x <= 1; ++x)
    {
        for (int y = -1; y <= 1; ++y)
        {
            vec2 offset = vec2(float(x), float(y)) * texelSize;
            shadow += texture(shadowMap, vec4(projCoords.xy + offset, layer,
                                              projCoords.z - bias));
        }
    }
    return shadow / 9.0;
}

void main()
{
    // Identity light-space matrix: projCoords.xy == v_TexCoord.yx swap? No,
    // we simply author projCoords directly from v_TexCoord for this probe.
    // Host maps v_TexCoord.x → depth sweep and v_TexCoord.y → XY sample.
    float depth = clamp(v_TexCoord.x, 0.0, 1.0);
    vec2  sampleXY = vec2(v_TexCoord.y, 0.5);

    vec3 projCoords = vec3(sampleXY, depth);
    float bias = u_ShadowParams.x;

    // Force cascade 0 — the surrounding cascade-selection / blend / fade
    // logic is exercised independently by ShaderUnit_ShadowBounds.glsl.
    float shadow = sampleShadowPCF(u_ShadowMapCSM, projCoords, 0.0, bias,
                                   u_ShadowResolution);

    o_Color = vec4(shadow, depth, bias, 1.0);
}

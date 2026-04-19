// =============================================================================
// ShaderUnit_ShadowBounds.glsl
//
// Math-only probe for the shadow bounds / cascade selection logic from
// PBRCommon.glsl::calculateCascadedShadowFactorCSM. We replicate the
// *non-texture* control-flow paths (out-of-frustum check, max-shadow-distance
// early-out, cascade index selection) and write a sentinel value per input
// so the test can assert the correct path was taken without needing a real
// depth-comparison sampler.
//
// Output encoding (R channel):
//   1.00 → "fully lit" path (boundary cases must all hit this)
//   0.25 → "past max shadow distance" early-out executed
//   0.50 → "projCoords out of frustum" early-out executed
//   0.75 → "would sample PCF" — interior cascade path (we fake a unity
//          shadow value since no real shadow map is bound)
//
// Cascade index (0..3) is written to G channel / 4 so tests can verify
// cascade selection math.
//
// Input grid (UV [0,1] mapped to the test fixture):
//   x axis → -viewDepth ∈ [0, maxShadowDistance * 1.2]  (x>1 = past max)
//   y axis → NDC position offset (bottom half in-frustum, top half pushed
//            out to NDC 2.0 → must short-circuit to "lit")
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

const float MAX_SHADOW_DISTANCE = 100.0;
const vec4  CASCADE_PLANES = vec4(10.0, 25.0, 50.0, 100.0);

// Replicates the control-flow of calculateCascadedShadowFactorCSM without
// sampling an actual shadow map.
float shadowBoundsPath(float viewDepth, vec3 projCoords, out int outCascade)
{
    outCascade = -1;
    if (-viewDepth > MAX_SHADOW_DISTANCE)
        return 0.25; // past max distance sentinel

    // Cascade selection (same logic as production).
    int cascadeIndex = 3;
    for (int i = 0; i < 4; ++i)
    {
        if (-viewDepth < CASCADE_PLANES[i])
        {
            cascadeIndex = i;
            break;
        }
    }
    outCascade = cascadeIndex;

    if (projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0 ||
        projCoords.z > 1.0)
    {
        return 0.50; // out-of-frustum sentinel
    }

    return 0.75; // would sample PCF
}

void main()
{
    // Map UV -> synthetic inputs.
    float viewDepthMag = mix(0.0, MAX_SHADOW_DISTANCE * 1.2, v_TexCoord.x);
    float viewDepth = -viewDepthMag;

    // Bottom half: well inside the shadow frustum ((0.5, 0.5, 0.5) is safe).
    // Top half: pushed out on X to (2.0, 0.5, 0.5) → out of bounds.
    vec3 projCoords;
    if (v_TexCoord.y < 0.5)
        projCoords = vec3(0.5, 0.5, 0.5);
    else
        projCoords = vec3(2.0, 0.5, 0.5);

    int cascade = -1;
    float pathSentinel = shadowBoundsPath(viewDepth, projCoords, cascade);

    // Encode cascade index (0..3) into G, mapped to [0, 1].
    float cascadeEncoded = (cascade + 1.0) / 5.0;

    o_Color = vec4(pathSentinel, cascadeEncoded, 0.0, 1.0);
}

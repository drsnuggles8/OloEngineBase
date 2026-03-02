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

layout(binding = 0) uniform sampler2D u_Texture;

#include "include/PrecipitationCommon.glsl"

// ── Screen-space precipitation UBO (binding 19, std140) ──
#define MAX_LENS_IMPACTS 16

struct LensImpactData
{
    vec4 PositionAndSize;   // xy = screenUV, z = size, w = rotation
    vec4 TimeParams;        // x = normalizedAge, y = fadeFactor, z = unused, w = active
};

layout(std140, binding = 19) uniform PrecipitationScreenUBO
{
    vec4 u_StreakParams;                           // xy = direction, z = intensity, w = length
    LensImpactData u_LensImpacts[MAX_LENS_IMPACTS]; // 16 impacts × 2 vec4 = 32 vec4
};

void main()
{
    vec3 sceneColor = texture(u_Texture, v_TexCoord).rgb;

    // ────────────────────────────────────────────────
    // 1. Directional snow streaks
    // ────────────────────────────────────────────────
    vec3 streakContribution = vec3(0.0);
    if (precipStreaksEnabled() && u_StreakParams.z > 0.001)
    {
        vec2 streakDir = u_StreakParams.xy;
        float streakIntensity = u_StreakParams.z;
        float streakLen = u_StreakParams.w;

        // Compute streak UV: project screen position onto streak direction
        float time = precipTime();

        // Animated directional noise — scrolls with wind
        vec2 scrollUV = v_TexCoord * 8.0; // Tile frequency
        scrollUV += streakDir * time * 2.0; // Scroll speed

        // Elongate noise along wind direction (streak effect)
        // Rotate UV so wind direction aligns with one axis, then scale
        float angle = atan(streakDir.y, streakDir.x);
        float cosA = cos(angle);
        float sinA = sin(angle);
        vec2 centered = v_TexCoord - 0.5;
        vec2 rotatedUV = vec2(
            centered.x * cosA + centered.y * sinA,
            -centered.x * sinA + centered.y * cosA
        );
        // Stretch along streak direction
        rotatedUV.x *= 1.0;
        rotatedUV.y *= max(streakLen * 4.0, 1.0);

        float streakNoise = precipFBM(rotatedUV * 12.0 + streakDir * time * 3.0);
        streakNoise = max(streakNoise, 0.0);
        streakNoise = pow(streakNoise, 2.0); // Sharpen

        // Fade out near screen edges
        vec2 edgeFade = smoothstep(vec2(0.0), vec2(0.15), v_TexCoord)
                      * smoothstep(vec2(0.0), vec2(0.15), 1.0 - v_TexCoord);
        float edgeMask = edgeFade.x * edgeFade.y;

        streakContribution = precipParticleColor().rgb * streakNoise * streakIntensity * edgeMask;
    }

    // ────────────────────────────────────────────────
    // 2. Lens snowflake impacts
    // ────────────────────────────────────────────────
    vec3 lensContribution = vec3(0.0);
    float lensDistortion = 0.0;

    for (int i = 0; i < MAX_LENS_IMPACTS; ++i)
    {
        if (u_LensImpacts[i].TimeParams.w < 0.5) // inactive
            continue;

        vec2 impactPos = u_LensImpacts[i].PositionAndSize.xy;
        float impactSize = u_LensImpacts[i].PositionAndSize.z;
        float impactRotation = u_LensImpacts[i].PositionAndSize.w;
        float normalizedAge = u_LensImpacts[i].TimeParams.x;
        float fadeFactor = u_LensImpacts[i].TimeParams.y;

        // Distance from this fragment to impact center
        vec2 delta = v_TexCoord - impactPos;
        float dist = length(delta);

        if (dist > impactSize * 2.0)
            continue;

        // Radial falloff
        float spotAlpha = precipLensSpotFalloff(dist, impactSize);

        // 6-fold snowflake crystal pattern
        float impactAngle = atan(delta.y, delta.x) - impactRotation;
        float crystalPattern = 0.7 + 0.3 * cos(impactAngle * 6.0);
        crystalPattern *= 0.8 + 0.2 * cos(impactAngle * 12.0); // Detail
        float normalizedDist = dist / impactSize;
        crystalPattern = mix(1.0, crystalPattern, smoothstep(0.2, 0.8, normalizedDist));

        // Melt animation: impact shrinks and gets more transparent over lifetime
        float meltScale = mix(1.0, 0.3, normalizedAge * normalizedAge);
        spotAlpha *= crystalPattern * fadeFactor * meltScale;
        spotAlpha = max(spotAlpha, 0.0);

        // Slight refraction/distortion through the ice
        lensDistortion += spotAlpha * 0.02 * fadeFactor;

        // White-ish tint with slight blue for ice
        vec3 impactColor = mix(vec3(0.85, 0.9, 1.0), vec3(1.0), normalizedAge);
        lensContribution += impactColor * spotAlpha * 0.3;
    }

    // Apply lens distortion to scene color (slight normal-map refraction effect)
    vec2 distortedUV = v_TexCoord;
    if (lensDistortion > 0.0001)
    {
        // Radial distortion from combined lens impacts
        vec2 center = vec2(0.5);
        vec2 dir = v_TexCoord - center;
        distortedUV = v_TexCoord + dir * lensDistortion;
        distortedUV = clamp(distortedUV, vec2(0.0), vec2(1.0));
        sceneColor = texture(u_Texture, distortedUV).rgb;
    }

    // ────────────────────────────────────────────────
    // 3. Composite
    // ────────────────────────────────────────────────
    vec3 finalColor = sceneColor + streakContribution + lensContribution;

    o_Color = vec4(finalColor, 1.0);
}

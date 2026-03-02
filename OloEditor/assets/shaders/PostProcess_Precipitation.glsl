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
    // 1. Directional precipitation streaks
    // ────────────────────────────────────────────────
    vec3 streakContribution = vec3(0.0);
    if (precipStreaksEnabled() && u_StreakParams.z > 0.001)
    {
        vec2 streakDir = u_StreakParams.xy;
        float streakIntensity = u_StreakParams.z;
        float streakLen = u_StreakParams.w;

        float time = precipTime();
        int pType = precipType();

        // Tile frequency — rain uses finer tiling for more streaks
        float tileFreq = (pType == PRECIP_RAIN) ? 16.0 : 8.0;
        vec2 scrollUV = v_TexCoord * tileFreq;
        float scrollSpeed = (pType == PRECIP_RAIN) ? 4.0 : 2.0;
        scrollUV += streakDir * time * scrollSpeed;

        // Elongate noise along wind direction
        float angle = atan(streakDir.y, streakDir.x);
        float cosA = cos(angle);
        float sinA = sin(angle);
        vec2 centered = v_TexCoord - 0.5;
        vec2 rotatedUV = vec2(
            centered.x * cosA + centered.y * sinA,
            -centered.x * sinA + centered.y * cosA
        );

        // Stretch factor — rain much more elongated than snow
        float stretchMult = (pType == PRECIP_RAIN) ? 8.0
                          : (pType == PRECIP_HAIL) ? 2.0
                          : (pType == PRECIP_SLEET) ? 5.0
                          : 4.0; // snow
        rotatedUV.x *= 1.0;
        rotatedUV.y *= max(streakLen * stretchMult, 1.0);

        float fbmFreq = (pType == PRECIP_RAIN) ? 18.0 : 12.0;
        float fbmScroll = (pType == PRECIP_RAIN) ? 6.0 : 3.0;
        float streakNoise = precipFBM(rotatedUV * fbmFreq + streakDir * time * fbmScroll);
        streakNoise = max(streakNoise, 0.0);
        // Rain: sharper, thinner streaks; Snow: softer; Hail: weakest
        float sharpness = (pType == PRECIP_RAIN) ? 3.0
                        : (pType == PRECIP_HAIL) ? 1.5
                        : 2.0;
        streakNoise = pow(streakNoise, sharpness);

        // Fade out near screen edges
        vec2 edgeFade = smoothstep(vec2(0.0), vec2(0.15), v_TexCoord)
                      * smoothstep(vec2(0.0), vec2(0.15), 1.0 - v_TexCoord);
        float edgeMask = edgeFade.x * edgeFade.y;

        streakContribution = precipParticleColor().rgb * streakNoise * streakIntensity * edgeMask;
    }

    // ────────────────────────────────────────────────
    // 2. Lens impacts (type-dependent pattern)
    // ────────────────────────────────────────────────
    vec3 lensContribution = vec3(0.0);
    float lensDistortion = 0.0;
    int pType = precipType();

    for (int i = 0; i < MAX_LENS_IMPACTS; ++i)
    {
        if (u_LensImpacts[i].TimeParams.w < 0.5) // inactive
            continue;

        vec2 impactPos = u_LensImpacts[i].PositionAndSize.xy;
        float impactSize = u_LensImpacts[i].PositionAndSize.z;
        float impactRotation = u_LensImpacts[i].PositionAndSize.w;
        float normalizedAge = u_LensImpacts[i].TimeParams.x;
        float fadeFactor = u_LensImpacts[i].TimeParams.y;

        vec2 delta = v_TexCoord - impactPos;
        float dist = length(delta);

        if (dist > impactSize * 2.0)
            continue;

        // Radial falloff
        float spotAlpha = precipLensSpotFalloff(dist, impactSize);
        float normalizedDist = dist / impactSize;

        float patternMod = 1.0;
        vec3 impactColor;

        if (pType == PRECIP_SNOW)
        {
            // 6-fold snowflake crystal
            float impactAngle = atan(delta.y, delta.x) - impactRotation;
            patternMod = 0.7 + 0.3 * cos(impactAngle * 6.0);
            patternMod *= 0.8 + 0.2 * cos(impactAngle * 12.0);
            patternMod = mix(1.0, patternMod, smoothstep(0.2, 0.8, normalizedDist));
            impactColor = mix(vec3(0.85, 0.9, 1.0), vec3(1.0), normalizedAge);
        }
        else if (pType == PRECIP_RAIN)
        {
            // Circular water droplet — ring pattern with refraction
            float ring = 1.0 - abs(normalizedDist - 0.6) * 4.0;
            ring = max(ring, 0.0);
            patternMod = mix(1.0, 0.5 + 0.5 * ring, smoothstep(0.1, 0.5, normalizedDist));
            impactColor = vec3(0.65, 0.72, 0.88); // Blue-ish water
            spotAlpha *= 0.7; // More transparent
            lensDistortion += spotAlpha * 0.04 * fadeFactor; // Stronger refraction
        }
        else if (pType == PRECIP_HAIL)
        {
            // Ice crack pattern — radial lines from center
            float impactAngle = atan(delta.y, delta.x);
            float crackBase = abs(sin(impactAngle * 8.0)); // 8 radial cracks
            float crack = pow(crackBase, 0.3); // Widen cracks
            float crackFade = smoothstep(0.0, 0.7, normalizedDist);
            patternMod = mix(1.0, 0.3 + 0.7 * crack, crackFade);
            impactColor = vec3(0.9, 0.93, 0.97); // Icy white
            spotAlpha *= 1.2; // More visible — hard ice
        }
        else // Sleet
        {
            // Mild 4-fold pattern between crystal and droplet
            float impactAngle = atan(delta.y, delta.x) - impactRotation;
            patternMod = 0.85 + 0.15 * cos(impactAngle * 4.0);
            patternMod = mix(1.0, patternMod, smoothstep(0.3, 0.8, normalizedDist));
            impactColor = mix(vec3(0.75, 0.8, 0.9), vec3(0.95), normalizedAge);
            lensDistortion += spotAlpha * 0.025 * fadeFactor;
        }

        // Melt/evaporation animation
        float meltScale = mix(1.0, 0.3, normalizedAge * normalizedAge);
        spotAlpha *= patternMod * fadeFactor * meltScale;
        spotAlpha = max(spotAlpha, 0.0);

        // Common refraction for snow (minimal)
        if (pType == PRECIP_SNOW)
        {
            lensDistortion += spotAlpha * 0.02 * fadeFactor;
        }

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

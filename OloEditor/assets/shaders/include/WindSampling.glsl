// WindSampling.glsl - Shared wind-field sampling utilities
// Include this file in any shader that needs to sample the 3D wind volume.
//
// Requirements:
//   - WindUBO (std140, binding = 15) must be declared or already bound
//   - u_WindField sampler3D must be bound to texture slot 29
//
// Usage:
//   vec3 wind = sampleWindField(worldPosition);

#ifndef WIND_SAMPLING_GLSL
#define WIND_SAMPLING_GLSL

// ---- Wind UBO (std140, binding 15) ----
layout(std140, binding = 15) uniform WindUBO
{
    vec4 w_DirectionAndSpeed;   // xyz = normalized direction, w = speed (m/s)
    vec4 w_GustAndTurbulence;   // x = GustStrength, y = GustFrequency, z = TurbulenceIntensity, w = TurbulenceScale
    vec4 w_GridMinAndSize;      // xyz = grid AABB min corner, w = grid world size (meters)
    vec4 w_TimeAndFlags;        // x = time (s), y = enabled (1.0/0.0), z = grid resolution, w = pad
};

layout(binding = 29) uniform sampler3D u_WindField;

// Convenience accessors
bool  windEnabled()       { return w_TimeAndFlags.y > 0.5; }
vec3  windDirection()     { return w_DirectionAndSpeed.xyz; }
float windSpeed()         { return w_DirectionAndSpeed.w; }
float windTime()          { return w_TimeAndFlags.x; }
float windGridSize()      { return w_GridMinAndSize.w; }

/**
 * @brief Sample the GPU-generated 3D wind field at a world position.
 *
 * Returns the interpolated wind velocity (m/s) from the 128³ volume.
 * Positions outside the grid are clamped to the boundary (GL_CLAMP_TO_EDGE).
 *
 * @param worldPos  World-space position to query.
 * @return vec3     Wind velocity at worldPos.  Zero if wind is disabled.
 */
vec3 sampleWindField(vec3 worldPos)
{
    if (!windEnabled())
        return vec3(0.0);

    vec3 gridMin = w_GridMinAndSize.xyz;
    float gridSize = w_GridMinAndSize.w;

    // World pos → [0,1] UVW within the grid cube
    vec3 uvw = (worldPos - gridMin) / gridSize;

    // texture() with GL_CLAMP_TO_EDGE handles out-of-bounds gracefully
    return texture(u_WindField, uvw).rgb;
}

/**
 * @brief Quick analytical wind approximation (no texture read).
 *
 * Uses only UBO data: base direction + gust sine.  Cheaper than a
 * texture fetch, suitable for vertex shaders or distant objects.
 *
 * @param worldPos  World-space position.
 * @return vec3     Approximate wind velocity.
 */
vec3 analyticalWind(vec3 worldPos)
{
    if (!windEnabled())
        return vec3(0.0);

    vec3 dir = w_DirectionAndSpeed.xyz;
    float speed = w_DirectionAndSpeed.w;
    float gustStr = w_GustAndTurbulence.x;
    float gustFreq = w_GustAndTurbulence.y;
    float time = w_TimeAndFlags.x;

    float gustPhase = time * gustFreq * 6.2831853;
    float spatial = dot(worldPos, dir) * 0.05;
    float gust = 1.0 + gustStr * sin(gustPhase + spatial);

    return dir * speed * gust;
}

#endif // WIND_SAMPLING_GLSL

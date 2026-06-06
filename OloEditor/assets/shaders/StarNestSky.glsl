// =============================================================================
// StarNestSky.glsl - Star Nest raymarched volumetric nebula sky
//
// Renders the "Star Nest" procedural nebula into one face of a cubemap; the
// cubemap is then consumed by the existing skybox + IBL pipeline, so reflective
// PBR surfaces, reflection probes and SSR all reflect the baked nebula for free.
//
// Reference: "Star Nest" by Pablo Roman Andrioli (Kali), MIT License.
//   https://www.shadertoy.com/view/XlfGRj
//
// Unlike the original full-screen shadertoy, the ray direction here is the
// per-face cubemap sample direction (the unit-cube vertex position), so the
// nebula is baked across all six faces of a seamless environment cubemap.
// Driven by StarNestSkyUBO (see StarNestSky.h) at UBO_STAR_NEST_SKY (39).
// =============================================================================

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;

layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};

layout(location = 0) out vec3 v_LocalPos;

void main()
{
    v_LocalPos = a_Position;
    gl_Position = u_ViewProjection * vec4(a_Position, 1.0);
}

#type fragment
#version 460 core

layout(location = 0) in vec3 v_LocalPos;
layout(location = 0) out vec4 o_Color;

// Must match ShaderBindingLayout::UBO_STAR_NEST_SKY (39) and StarNestSkyUBO.
layout(std140, binding = 39) uniform StarNestSkyData {
    vec4 u_Offset;   // xyz = camera position in the nebula field, w = rotation1 (xz plane, radians)
    vec4 u_Params0;  // x = formuparam, y = stepsize, z = tile, w = rotation2 (xy plane, radians)
    vec4 u_Params1;  // x = brightness, y = darkmatter, z = distfading, w = saturation
    vec4 u_Params2;  // x = intensity, y = iterations (float), z = volsteps (float), w = unused
};

// Fixed upper bounds so the loops have compile-time-constant trip counts
// (required for robust shaderc -> SPIR-V translation); the runtime counts read
// from the UBO break early. These ceilings also cap the worst-case bake cost.
const int MAX_ITERATIONS = 40;
const int MAX_VOLSTEPS = 40;

void main()
{
    // The unit-cube vertex direction is the cubemap sample ray.
    vec3 dir = normalize(v_LocalPos);

    // Orient the whole nebula via two plane rotations (matches the reference's
    // mouse rotation, exposed here as authoring parameters).
    float a1 = u_Offset.w;
    float a2 = u_Params0.w;
    mat2 rot1 = mat2(cos(a1), sin(a1), -sin(a1), cos(a1));
    mat2 rot2 = mat2(cos(a2), sin(a2), -sin(a2), cos(a2));
    dir.xz = dir.xz * rot1;
    dir.xy = dir.xy * rot2;

    vec3 from = u_Offset.xyz;
    from.xz = from.xz * rot1;
    from.xy = from.xy * rot2;

    float formuparam = u_Params0.x;
    float stepsize = u_Params0.y;
    float tile = u_Params0.z;
    float brightness = u_Params1.x;
    float darkmatter = u_Params1.y;
    float distfading = u_Params1.z;
    float saturation = u_Params1.w;
    float intensity = u_Params2.x;
    int iterations = int(u_Params2.y);
    int volsteps = int(u_Params2.z);

    // Volumetric march through the folded fractal field.
    float s = 0.1;
    float fade = 1.0;
    vec3 v = vec3(0.0);
    for (int r = 0; r < MAX_VOLSTEPS; r++)
    {
        if (r >= volsteps)
            break;

        vec3 p = from + s * dir * 0.5;
        p = abs(vec3(tile) - mod(p, vec3(tile * 2.0))); // tiling fold

        float pa = 0.0;
        float a = 0.0;
        for (int i = 0; i < MAX_ITERATIONS; i++)
        {
            if (i >= iterations)
                break;
            // The magic formula. Guard dot(p,p) against zero so a degenerate
            // fold can't emit inf/NaN into the cubemap (a single NaN texel
            // poisons the whole IBL irradiance convolution).
            p = abs(p) / max(dot(p, p), 1e-6) - formuparam;
            a += abs(length(p) - pa); // absolute sum of average change
            pa = length(p);
        }

        float dm = max(0.0, darkmatter - a * a * 0.001); // dark matter
        a *= a * a;                                       // add contrast
        if (r > 6)
            fade *= 1.0 - dm; // dark matter: don't render near
        v += fade;
        v += vec3(s, s * s, s * s * s * s) * a * brightness * fade; // distance coloring
        fade *= distfading;                                          // distance fading
        s += stepsize;
    }

    // Desaturate toward greyscale by `1 - saturation`.
    v = mix(vec3(length(v)), v, saturation);

    // Match the reference's final scale, then apply the authoring intensity.
    vec3 color = max(v * 0.01 * intensity, vec3(0.0));
    o_Color = vec4(color, 1.0);
}

// =============================================================================
// Impostor_Bake.glsl — bakes one octahedral view-angle slice of a mesh into an
// impostor atlas tile. Rendered with an ORTHOGRAPHIC camera per tile (view-proj
// supplied on the CPU). Two MRT outputs:
//   RT0 = albedo.rgb, coverage.a
//   RT1 = object-space normal (rgb, *0.5+0.5), card-relative depth (a, 0.5 = card plane)
// Object space, +Y up. See issue #433 / ImpostorBaker.cpp.
// =============================================================================

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord;

layout(std140, binding = 55) uniform ImpostorBakeParams
{
    mat4 u_BakeViewProjection;
    vec4 u_BakeCenterRadius; // xyz = object-space bounding-sphere center, w = radius
    vec4 u_BakeDirCutoff;    // xyz = capture view dir (object -> camera), w = alpha cutoff
    vec4 u_BakeTint;         // rgb = tint colour, a = unused
};

layout(location = 0) out vec3 v_ObjPos;
layout(location = 1) out vec3 v_Normal;
layout(location = 2) out vec2 v_TexCoord;

void main()
{
    v_ObjPos = a_Position;
    v_Normal = a_Normal; // object space
    v_TexCoord = a_TexCoord;
    gl_Position = u_BakeViewProjection * vec4(a_Position, 1.0);
}

#type fragment
#version 460 core

layout(location = 0) out vec4 o_Albedo;      // rgb = albedo, a = coverage
layout(location = 1) out vec4 o_NormalDepth; // rgb = obj normal encoded, a = depth

layout(location = 0) in vec3 v_ObjPos;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) in vec2 v_TexCoord;

layout(std140, binding = 55) uniform ImpostorBakeParams
{
    mat4 u_BakeViewProjection;
    vec4 u_BakeCenterRadius;
    vec4 u_BakeDirCutoff;
    vec4 u_BakeTint;
};

layout(binding = 0) uniform sampler2D u_AlbedoTexture;

void main()
{
    vec4 tex = texture(u_AlbedoTexture, v_TexCoord);
    vec3 albedo = tex.rgb * u_BakeTint.rgb;
    float coverage = tex.a;

    // Hard cutout so only opaque fragments write depth into the tile — a
    // transparent leaf quad in front must not occlude opaque geometry behind it.
    float cutoff = u_BakeDirCutoff.w;
    if (coverage < cutoff)
        discard;

    // Object-space normal, two-sided (leaves are single-sided quads; face the
    // capture camera so the baked normal always has a sensible hemisphere).
    vec3 n = normalize(v_Normal);
    vec3 viewDir = normalize(u_BakeDirCutoff.xyz);
    if (dot(n, viewDir) < 0.0)
        n = -n;

    // Card-relative depth: signed distance from the card plane (through the
    // bounding-sphere centre, normal = viewDir) toward the camera, normalized so
    // 0.5 == card plane, < 0.5 == closer to camera.
    vec3 center = u_BakeCenterRadius.xyz;
    float radius = max(u_BakeCenterRadius.w, 1e-4);
    float signedDist = dot(v_ObjPos - center, viewDir); // toward camera positive
    float depth = clamp(0.5 - signedDist / (2.0 * radius), 0.0, 1.0);

    o_Albedo = vec4(albedo, coverage);
    o_NormalDepth = vec4(n * 0.5 + 0.5, depth);
}

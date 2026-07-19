// =============================================================================
// Foliage_Impostor.glsl — octahedral impostor card for distant foliage (issue
// #433). Replaces the flat Y-rotated billboard with a fully camera-facing card
// that samples a baked octahedral atlas (albedo + object-normal + depth),
// blending the 3 lattice frames around the current view direction (no slice
// pop), applying single-step depth parallax (kills the flat-card look), and
// relighting from the baked object normal. A distance-driven detail ramp across
// [ImpostorStart, ImpostorStart+Band] fades parallax + cross-frame blend in with
// range so there is no visible transition. Forward pass: composites into
// SceneColor after opaque.
// =============================================================================

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;

layout(location = 2) in vec4 a_PositionScale;  // xyz = world pos, w = scale
layout(location = 3) in vec4 a_RotationHeight; // x = Y rotation (rad), y = height, z = fade, w = unused
layout(location = 4) in vec4 a_ColorAlpha;     // rgb = tint, a = alpha cutoff

layout(std140, binding = 0) uniform CameraMatrices
{
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
    mat4 u_PrevViewProjection;
    vec3 u_RenderOrigin;
    float _padding1;
};

#include "include/InstanceBlock_Vertex.glsl"

layout(std140, binding = 12) uniform FoliageParams
{
    float u_Time;
    float u_WindStrength;
    float u_WindSpeed;
    float u_ViewDistance;
    float u_FadeStart;
    float u_AlphaCutoff;
    float u_PrevTime;
    float _foliagePad1;
    vec3 u_FoliageBaseColor;
    float _foliagePad2;
    vec4 u_ImpostorParams0; // x=framesPerAxis, y=hemi, z=startDistance, w=transitionBand
    vec4 u_ImpostorParams1; // x=enabled, y=meshRadius, z=parallaxScale, w=unused
};

layout(location = 0) out vec3 v_CardWorld;  // this fragment's card world position
layout(location = 1) out vec3 v_PivotWorld; // card centre (world)
layout(location = 2) out vec2 v_Uv;         // card uv 0..1
layout(location = 3) out vec3 v_Color;      // tint
layout(location = 4) out float v_AlphaCutoff;
layout(location = 5) out float v_Rotation;  // instance Y rotation
layout(location = 6) out vec3 v_PrevCardWorld;
layout(location = 7) out float v_Radius;    // WORLD-space card radius (object radius * scale)

void main()
{
    OLO_INSTANCE_FORWARD();

    float scale = a_PositionScale.w;
    float rotation = a_RotationHeight.x;
    float radius = u_ImpostorParams1.y * scale;  // world-space card radius

    // Instance pivot. Foliage's per-instance positions are absolute world in
    // a_PositionScale; the foliage command uploads only a SINGLE model-matrix
    // entry to the InstanceData SSBO, so indexing it by gl_InstanceIndex for
    // instance > 0 reads out of bounds (zero matrix on this driver) and would
    // collapse every card to the origin. Shift to render-relative space directly
    // (subtract the render origin) instead of going through u_Model (issue #433).
    vec3 instWorld = a_PositionScale.xyz - u_RenderOrigin;
    // Anchor the impostor so its bounding sphere RESTS ON the ground: centre one
    // world radius above the instance ground point. This is independent of how
    // the source mesh is authored (centred-on-origin vs base-at-origin) — using
    // the baked centre.y directly would half-bury a centred mesh, and a
    // camera-looking-down view then flattens that half-buried card into the
    // terrain and depth-culls it (issue #433).
    vec3 cardCenter = instWorld + vec3(0.0, radius, 0.0);

    // Subtle whole-card wind sway (legacy sine model, matching the near
    // billboard's fallback branch) so a distant tree still moves with the wind.
    float windPhase = (a_PositionScale.x + a_PositionScale.z) * 0.1 + u_Time * u_WindSpeed;
    float windPhasePrev = (a_PositionScale.x + a_PositionScale.z) * 0.1 + u_PrevTime * u_WindSpeed;
    float sway = sin(windPhase) * cos(windPhase * 0.7 + 1.3) * u_WindStrength * 0.15;
    float swayPrev = sin(windPhasePrev) * cos(windPhasePrev * 0.7 + 1.3) * u_WindStrength * 0.15;
    vec3 cardCenterCur = cardCenter + vec3(sway, 0.0, sway * 0.5);
    vec3 cardCenterPrev = cardCenter + vec3(swayPrev, 0.0, swayPrev * 0.5);

    // Camera-facing basis. u_CameraPosition is treated in the same space as the
    // render-relative pivot (renderOrigin ~ 0 for authored scenes) — matches the
    // existing foliage distance/fade convention.
    vec3 toCam = u_CameraPosition - cardCenterCur;
    vec3 zAxis = normalize(toCam);
    vec3 upRef = (abs(zAxis.y) > 0.999) ? vec3(0.0, 0.0, -1.0) : vec3(0.0, 1.0, 0.0);
    vec3 xAxis = normalize(cross(upRef, zAxis));
    vec3 yAxis = normalize(cross(zAxis, xAxis));

    vec2 offset = (a_TexCoord - 0.5) * (2.0 * radius);
    vec3 cardWorld = cardCenterCur + xAxis * offset.x + yAxis * offset.y;
    vec3 cardWorldPrev = cardCenterPrev + xAxis * offset.x + yAxis * offset.y;

    v_CardWorld = cardWorld;
    v_PrevCardWorld = cardWorldPrev;
    v_PivotWorld = cardCenterCur;
    v_Uv = a_TexCoord;
    v_Color = a_ColorAlpha.rgb;
    v_AlphaCutoff = a_ColorAlpha.a;
    v_Rotation = rotation;
    v_Radius = radius; // world-space half-size, needed by the fragment's virtual-plane UV

    gl_Position = u_ViewProjection * vec4(cardWorld, 1.0);
}

#type fragment
#version 460 core

layout(location = 0) out vec4 FragColor;
layout(location = 3) out vec2 o_Velocity;

layout(location = 0) in vec3 v_CardWorld;
layout(location = 1) in vec3 v_PivotWorld;
layout(location = 2) in vec2 v_Uv;
layout(location = 3) in vec3 v_Color;
layout(location = 4) in float v_AlphaCutoff;
layout(location = 5) in float v_Rotation;
layout(location = 6) in vec3 v_PrevCardWorld;
layout(location = 7) in float v_Radius; // WORLD-space card radius

layout(std140, binding = 0) uniform CameraMatrices
{
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
    mat4 u_PrevViewProjection;
    vec3 u_RenderOrigin;
    float _padding1;
};

layout(std140, binding = 5) uniform MultiLightData
{
    int u_NumLights;
    int _ml_pad0;
    int _ml_pad1;
    int _ml_pad2;
    vec4 u_Light0_Position;
    vec4 u_Light0_Direction;
    vec4 u_Light0_ColorIntensity;
    vec4 u_Light0_Params;
    vec4 u_Light0_Params2;
};

layout(std140, binding = 12) uniform FoliageParams
{
    float u_Time;
    float u_WindStrength;
    float u_WindSpeed;
    float u_ViewDistance;
    float u_FadeStart;
    float u_AlphaCutoff;
    float u_PrevTime;
    float _foliagePad1;
    vec3 u_FoliageBaseColor;
    float _foliagePad2;
    vec4 u_ImpostorParams0; // x=framesPerAxis, y=hemi, z=startDistance, w=transitionBand
    vec4 u_ImpostorParams1; // x=enabled, y=meshRadius, z=parallaxScale, w=unused
};

layout(binding = 0) uniform sampler2D u_AlbedoAtlas;      // rgb=albedo, a=coverage
layout(binding = 10) uniform sampler2D u_NormalDepthAtlas; // rgb=obj normal, a=depth

#include "include/OctahedralImpostor.glsl"

// Rotate a vector about +Y by angle.
vec3 rotateY(vec3 v, float angle)
{
    float c = cos(angle);
    float s = sin(angle);
    return vec3(v.x * c + v.z * s, v.y, -v.x * s + v.z * c);
}

void main()
{
    float framesPerAxis = max(u_ImpostorParams0.x, 2.0);
    bool hemi = u_ImpostorParams0.y > 0.5;
    // WORLD-space card half-size — the virtual-plane offset vectors below are in
    // world units, so they must be divided by the world radius (object radius *
    // instance scale), NOT the object-space atlas radius.
    float radius = max(v_Radius, 1e-4);

    // Distance-driven detail ramp: near [< start] the card behaves like a cheap
    // stable single-frame billboard (no parallax, no cross-frame blend); across
    // [start, start+band] it cross-fades continuously to the full parallax
    // octahedral impostor — no pop, and both authoring knobs stay meaningful.
    float dist = distance(v_PivotWorld, u_CameraPosition);
    float lod = (u_ImpostorParams1.x > 0.5)
                    ? smoothstep(u_ImpostorParams0.z, u_ImpostorParams0.z + max(u_ImpostorParams0.w, 1e-3), dist)
                    : 1.0;
    float parallaxScale = u_ImpostorParams1.z * lod;

    // View direction in mesh-local space (undo the instance Y rotation).
    vec3 viewWorld = normalize(u_CameraPosition - v_PivotWorld);
    vec3 viewLocal = rotateY(viewWorld, -v_Rotation);

    vec2 grid = OctaDirToGrid(viewLocal, framesPerAxis, hemi);
    vec2 gridFloor = min(floor(grid), vec2(framesPerAxis - 1.0));
    vec2 f = grid - gridFloor;

    // 3-tile barycentric blend (quadBlendWeights); the two off-frames ramp in
    // with lod so the near look collapses to the single dominant frame.
    float w0 = min(1.0 - f.x, 1.0 - f.y);
    float w1 = abs(f.x - f.y) * lod;
    float w2 = min(f.x, f.y) * lod;
    float wsum = max(w0 + w1 + w2, 1e-4);
    w0 /= wsum;
    w1 /= wsum;
    w2 /= wsum;
    vec2 diag = (f.x > f.y) ? vec2(1.0, 0.0) : vec2(0.0, 1.0);
    vec2 maxFrame = vec2(framesPerAxis - 1.0);
    vec2 frame0 = gridFloor;
    vec2 frame1 = clamp(gridFloor + diag, vec2(0.0), maxFrame);
    vec2 frame2 = clamp(gridFloor + vec2(1.0, 1.0), vec2(0.0), maxFrame);

    vec2 frames[3] = vec2[3](frame0, frame1, frame2);
    float weights[3] = float[3](w0, w1, w2);

    float invN = 1.0 / framesPerAxis;
    vec3 pivotToCam = u_CameraPosition - v_PivotWorld;
    vec3 vertexToCam = u_CameraPosition - v_CardWorld;

    vec3 accAlbedo = vec3(0.0);
    vec3 accNormal = vec3(0.0);
    float accCoverage = 0.0;

    for (int i = 0; i < 3; ++i)
    {
        float weight = weights[i];
        if (weight <= 0.0)
            continue;

        // The direction this frame was captured from, back in world space.
        vec3 dirLocal = OctaFrameToDir(frames[i], framesPerAxis, hemi);
        vec3 dirWorld = rotateY(dirLocal, v_Rotation);

        // Virtual-plane reprojection: project the card ray onto this frame's
        // capture plane so each frame samples its own geometrically-correct UV
        // (otherwise the 3-frame blend ghosts). See issue #433 research.
        vec3 planeN = dirWorld;
        vec3 planeUp = (abs(planeN.y) > 0.999) ? vec3(0.0, 0.0, -1.0) : vec3(0.0, 1.0, 0.0);
        vec3 planeX = normalize(cross(planeUp, planeN));
        vec3 planeY = normalize(cross(planeN, planeX));

        // Intersect the camera->card-vertex ray with this frame's capture plane
        // (through the pivot, normal planeN): X = camera - offLen*vertexToCam, and
        // the in-plane offset from the pivot is pivotToCam - offLen*vertexToCam.
        float denom = dot(planeN, vertexToCam);
        if (abs(denom) < 1e-5)
            continue;
        float offLen = dot(planeN, pivotToCam) / denom;
        vec3 offVec = pivotToCam - vertexToCam * offLen;
        vec2 uvFrame = vec2(dot(planeX, offVec), dot(planeY, offVec)) / (2.0 * radius) + 0.5;

        // Single-step depth parallax along the in-plane view direction.
        vec2 tileUV = (frames[i] + clamp(uvFrame, 0.0, 1.0)) * invN;
        float depth = texture(u_NormalDepthAtlas, tileUV).a;
        vec2 viewTan = vec2(dot(planeX, viewWorld), dot(planeY, viewWorld));
        uvFrame += viewTan * (0.5 - depth) * parallaxScale;

        vec2 finalUV = (frames[i] + clamp(uvFrame, 0.0, 1.0)) * invN;
        vec4 alb = texture(u_AlbedoAtlas, finalUV);
        vec4 nd = texture(u_NormalDepthAtlas, finalUV);

        accAlbedo += alb.rgb * weight;
        accCoverage += alb.a * weight;
        accNormal += (nd.rgb * 2.0 - 1.0) * weight;
    }

    // The atlas albedo already has the layer tint baked in (ImpostorBaker applies
    // BaseColor at bake time) — do NOT re-multiply by v_Color here or the tint
    // is applied twice and the card reads far too dark.
    vec3 albedo = accAlbedo;
    float coverage = accCoverage;

    // Alpha test against the baked coverage.
    if (coverage < v_AlphaCutoff)
        discard;

    // Distance fade (matches the flat-billboard path).
    float distFade = 1.0 - smoothstep(u_FadeStart, u_ViewDistance, dist);
    if (distFade <= 0.0)
        discard;

    // Relight from the baked object-space normal (dynamic sun direction).
    vec3 localN = normalize(accNormal);
    vec3 worldN = normalize(rotateY(localN, v_Rotation));
    vec3 lightDir = normalize(-u_Light0_Direction.xyz);
    float NdotL = max(dot(worldN, lightDir), 0.0);
    if (NdotL < 0.01)
        NdotL = max(dot(-worldN, lightDir), 0.0) * 0.5; // two-sided foliage

    vec3 lightColor = u_Light0_ColorIntensity.rgb * u_Light0_ColorIntensity.w;
    vec3 ambient = albedo * 0.3;
    vec3 diffuse = albedo * lightColor * NdotL;
    vec3 litColor = ambient + diffuse;

    FragColor = vec4(litColor, coverage * distFade);

    // Camera-motion velocity (impostor has no per-instance prev history).
    vec4 clipCurr = u_ViewProjection * vec4(v_CardWorld, 1.0);
    vec4 clipPrev = u_PrevViewProjection * vec4(v_PrevCardWorld, 1.0);
    vec2 ndcCurr = clipCurr.xy / clipCurr.w;
    vec2 ndcPrev = clipPrev.xy / clipPrev.w;
    o_Velocity = (ndcCurr - ndcPrev) * 0.5;
}

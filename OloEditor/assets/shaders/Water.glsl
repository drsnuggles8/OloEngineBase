// =============================================================================
// Water.glsl - Gerstner wave water surface rendering
// Vertex-displaced water plane with Fresnel reflection, SSR, tessellation
// =============================================================================

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord;

// Camera UBO (binding 0)
layout(std140, binding = 0) uniform CameraMatrices
{
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
    // Previous-frame VP for scene FB RT3 velocity. Water waves are time-
    // varying so wave displacement itself is *not* reprojected (that would
    // need a prev-time uniform to re-evaluate the Gerstner sum); camera and
    // per-object transform motion are captured, matching the mesh-level
    // fidelity the rest of the forward path produces.
    mat4 u_PrevViewProjection;
};

// Model UBO (binding 3)
layout(std140, binding = 3) uniform ModelMatrices
{
    mat4 u_Model;
    mat4 u_Normal;
    int u_EntityID;
    int _paddingEntity0;
    int _paddingEntity1;
    int _paddingEntity2;
    mat4 u_PrevModel;
};

// Water UBO (binding 23)
layout(std140, binding = 23) uniform WaterParams
{
    vec4 u_WaveParams;             // x = Time, y = WaveSpeed, z = WaveAmplitude, w = WaveFrequency
    vec4 u_WaveDir0;               // xy = direction0, z = steepness0, w = wavelength0
    vec4 u_WaveDir1;               // xy = direction1, z = steepness1, w = wavelength1
    vec4 u_WaterColor;             // rgb = shallow color, a = Transparency
    vec4 u_WaterDeepColor;         // rgb = deep color,    a = Reflectivity
    vec4 u_VisualParams;           // x = FresnelPower, y = SpecularIntensity, z = NormalMapTiling, w = NoiseIntensity
    vec4 u_NormalMapScroll;        // xy = scroll0 offset, zw = scroll1 offset
    vec4 u_NormalMapSpeed;         // x = speed0, y = speed1, z/w = unused
    vec4 u_LightDirection;         // xyz = directional light dir (normalized), w = unused
    vec4 u_ScreenParams;           // x = width, y = height, z = 1/width, w = 1/height
    vec4 u_DepthRefractionParams;  // x = depthSoftening, y = refrDistortion, z = refrHeightFactor, w = unused
    vec4 u_RefractionColor;        // rgb = tint color, w = unused
    vec4 u_FoamParams;             // x = heightStart, y = fadeDistance, z = tiling, w = brightness
    vec4 u_FoamParams2;            // x = angleExponent, y = shorelinePower, z = sssIntensity, w = unused
    vec4 u_SSSColor;               // rgb = subsurface color, w = unused
    vec4 u_SSRParams;              // x = maxSteps (0=disabled), y = stepSize, z = maxDistance, w = thickness
    vec4 u_TessParams;             // x = tessellationFactor (0=disabled), y = minTessDist, z = maxTessDist, w = unused
};

#include "include/WaterCommon.glsl"

layout(location = 0) out vec3 v_WorldPos;
layout(location = 1) out vec3 v_Normal;
layout(location = 2) out vec2 v_TexCoord;
layout(location = 3) out vec3 v_ViewDir;
layout(location = 4) out vec3 v_Tangent;
layout(location = 5) out vec3 v_Bitangent;
layout(location = 6) out float v_WaveHeight;

void main()
{
    vec4 worldPos = u_Model * vec4(a_Position, 1.0);

    float time = u_WaveParams.x * u_WaveParams.y; // Time * WaveSpeed
    float amplitude = u_WaveParams.z;
    float frequency = u_WaveParams.w;

    // When tessellation is active, vertex shader passes through
    // (displacement happens in TES instead)
    if (u_TessParams.x > 0.0)
    {
        v_WorldPos = worldPos.xyz;
        v_Normal = vec3(0.0, 1.0, 0.0);
        v_TexCoord = a_TexCoord;
        v_ViewDir = normalize(u_CameraPosition - worldPos.xyz);
        v_Tangent = vec3(1.0, 0.0, 0.0);
        v_Bitangent = vec3(0.0, 0.0, 1.0);
        v_WaveHeight = 0.0;
        gl_Position = vec4(worldPos.xyz, 1.0); // TES will transform
        return;
    }

    vec3 displacedNormal;
    vec3 displacedPos = sumGerstnerWaves(
        worldPos.xyz, time,
        u_WaveDir0, u_WaveDir1,
        frequency, amplitude,
        displacedNormal
    );

    // Normalized wave height [-1, 1] for foam/SSS
    float maxAmplitude = max(amplitude, 0.001);
    v_WaveHeight = (displacedPos.y - worldPos.y) / maxAmplitude;

    v_WorldPos = displacedPos;
    v_Normal = displacedNormal;
    v_TexCoord = a_TexCoord;
    v_ViewDir = normalize(u_CameraPosition - displacedPos);

    // Compute tangent frame from displaced normal for normal mapping
    vec3 N = normalize(displacedNormal);
    vec3 up = abs(N.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    v_Tangent = normalize(cross(up, N));
    v_Bitangent = cross(N, v_Tangent);

    gl_Position = u_ViewProjection * vec4(displacedPos, 1.0);
}

// =============================================================================
// Tessellation Control Shader — adaptive tessellation for water surface
// =============================================================================
#type tess_control
#version 460 core

layout(vertices = 3) out;

layout(std140, binding = 0) uniform CameraMatrices
{
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
    mat4 u_PrevViewProjection;
};

layout(std140, binding = 23) uniform WaterParams
{
    vec4 u_WaveParams;
    vec4 u_WaveDir0;
    vec4 u_WaveDir1;
    vec4 u_WaterColor;
    vec4 u_WaterDeepColor;
    vec4 u_VisualParams;
    vec4 u_NormalMapScroll;
    vec4 u_NormalMapSpeed;
    vec4 u_LightDirection;
    vec4 u_ScreenParams;
    vec4 u_DepthRefractionParams;
    vec4 u_RefractionColor;
    vec4 u_FoamParams;
    vec4 u_FoamParams2;
    vec4 u_SSSColor;
    vec4 u_SSRParams;
    vec4 u_TessParams;
};

layout(location = 0) in vec3 v_WorldPos[];
layout(location = 1) in vec3 v_Normal[];
layout(location = 2) in vec2 v_TexCoord[];
layout(location = 3) in vec3 v_ViewDir[];
layout(location = 4) in vec3 v_Tangent[];
layout(location = 5) in vec3 v_Bitangent[];
layout(location = 6) in float v_WaveHeight[];

layout(location = 0) out vec3 tc_WorldPos[];
layout(location = 1) out vec3 tc_Normal[];
layout(location = 2) out vec2 tc_TexCoord[];

float calcTessLevel(vec3 p0, vec3 p1)
{
    float maxFactor = u_TessParams.x;
    float minDist = u_TessParams.y;
    float maxDist = u_TessParams.z;

    vec3 mid = (p0 + p1) * 0.5;
    float dist = distance(u_CameraPosition, mid);

    // Linear falloff: close = maxFactor, far = 1.0
    float t = clamp((dist - minDist) / max(maxDist - minDist, 0.001), 0.0, 1.0);
    return mix(maxFactor, 1.0, t);
}

void main()
{
    tc_WorldPos[gl_InvocationID] = v_WorldPos[gl_InvocationID];
    tc_Normal[gl_InvocationID] = v_Normal[gl_InvocationID];
    tc_TexCoord[gl_InvocationID] = v_TexCoord[gl_InvocationID];

    if (gl_InvocationID == 0)
    {
        float e0 = calcTessLevel(v_WorldPos[1], v_WorldPos[2]);
        float e1 = calcTessLevel(v_WorldPos[2], v_WorldPos[0]);
        float e2 = calcTessLevel(v_WorldPos[0], v_WorldPos[1]);

        gl_TessLevelOuter[0] = e0;
        gl_TessLevelOuter[1] = e1;
        gl_TessLevelOuter[2] = e2;
        gl_TessLevelInner[0] = (e0 + e1 + e2) / 3.0;
    }
}

// =============================================================================
// Tessellation Evaluation Shader — Gerstner displacement on subdivided mesh
// =============================================================================
#type tess_evaluation
#version 460 core

layout(triangles, equal_spacing, ccw) in;

layout(std140, binding = 0) uniform CameraMatrices
{
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
    mat4 u_PrevViewProjection;
};

layout(std140, binding = 3) uniform ModelMatrices
{
    mat4 u_Model;
    mat4 u_Normal;
    int u_EntityID;
    int _paddingEntity0;
    int _paddingEntity1;
    int _paddingEntity2;
    mat4 u_PrevModel;
};

layout(std140, binding = 23) uniform WaterParams
{
    vec4 u_WaveParams;
    vec4 u_WaveDir0;
    vec4 u_WaveDir1;
    vec4 u_WaterColor;
    vec4 u_WaterDeepColor;
    vec4 u_VisualParams;
    vec4 u_NormalMapScroll;
    vec4 u_NormalMapSpeed;
    vec4 u_LightDirection;
    vec4 u_ScreenParams;
    vec4 u_DepthRefractionParams;
    vec4 u_RefractionColor;
    vec4 u_FoamParams;
    vec4 u_FoamParams2;
    vec4 u_SSSColor;
    vec4 u_SSRParams;
    vec4 u_TessParams;
};

#include "include/WaterCommon.glsl"

layout(location = 0) in vec3 tc_WorldPos[];
layout(location = 1) in vec3 tc_Normal[];
layout(location = 2) in vec2 tc_TexCoord[];

layout(location = 0) out vec3 v_WorldPos;
layout(location = 1) out vec3 v_Normal;
layout(location = 2) out vec2 v_TexCoord;
layout(location = 3) out vec3 v_ViewDir;
layout(location = 4) out vec3 v_Tangent;
layout(location = 5) out vec3 v_Bitangent;
layout(location = 6) out float v_WaveHeight;

void main()
{
    // Barycentric interpolation
    vec3 pos = gl_TessCoord.x * tc_WorldPos[0]
             + gl_TessCoord.y * tc_WorldPos[1]
             + gl_TessCoord.z * tc_WorldPos[2];
    vec2 uv = gl_TessCoord.x * tc_TexCoord[0]
            + gl_TessCoord.y * tc_TexCoord[1]
            + gl_TessCoord.z * tc_TexCoord[2];

    // Apply Gerstner wave displacement
    float time = u_WaveParams.x * u_WaveParams.y;
    float amplitude = u_WaveParams.z;
    float frequency = u_WaveParams.w;

    vec3 displacedNormal;
    vec3 displacedPos = sumGerstnerWaves(
        pos, time,
        u_WaveDir0, u_WaveDir1,
        frequency, amplitude,
        displacedNormal
    );

    float maxAmplitude = max(amplitude, 0.001);
    v_WaveHeight = (displacedPos.y - pos.y) / maxAmplitude;

    v_WorldPos = displacedPos;
    v_Normal = displacedNormal;
    v_TexCoord = uv;
    v_ViewDir = normalize(u_CameraPosition - displacedPos);

    // Compute tangent frame
    vec3 N = normalize(displacedNormal);
    vec3 up = abs(N.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    v_Tangent = normalize(cross(up, N));
    v_Bitangent = cross(N, v_Tangent);

    gl_Position = u_ViewProjection * vec4(displacedPos, 1.0);
}

#type fragment
#version 460 core

#include "include/PBRCommon.glsl"

layout(location = 0) in vec3 v_WorldPos;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) in vec2 v_TexCoord;
layout(location = 3) in vec3 v_ViewDir;
layout(location = 4) in vec3 v_Tangent;
layout(location = 5) in vec3 v_Bitangent;
layout(location = 6) in float v_WaveHeight;

// MRT outputs matching SceneRenderPass format
layout(location = 0) out vec4 o_Color;
layout(location = 1) out int o_EntityID;
layout(location = 2) out vec2 o_ViewNormal;
// Scene FB RT3 velocity. We emit camera-motion-only velocity using the
// current-frame displaced world position for both current and previous
// clip-space computations — wave displacement itself is *not* reprojected
// (would require a prev-time uniform to re-sum Gerstner waves). This
// matches the mesh-level fidelity the forward path produces; TAA falls
// back to neighborhood clipping on pure wave motion, which is fine for
// the relatively slow Gerstner animation frequency.
layout(location = 3) out vec2 o_Velocity;

// Octahedral encode: unit normal -> RG16F [-1,1]^2
vec2 octEncode(vec3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return n.xy;
}

// Camera UBO (binding 0)
layout(std140, binding = 0) uniform CameraMatrices
{
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
    mat4 u_PrevViewProjection;
};

// Model UBO (binding 3) for entity ID
layout(std140, binding = 3) uniform ModelMatrices
{
    mat4 u_Model;
    mat4 u_Normal;
    int u_EntityID;
    int _paddingEntity0;
    int _paddingEntity1;
    int _paddingEntity2;
    mat4 u_PrevModel;
};

// Water UBO (binding 23)
layout(std140, binding = 23) uniform WaterParams
{
    vec4 u_WaveParams;
    vec4 u_WaveDir0;
    vec4 u_WaveDir1;
    vec4 u_WaterColor;             // rgb = shallow, a = transparency
    vec4 u_WaterDeepColor;         // rgb = deep,    a = reflectivity
    vec4 u_VisualParams;           // x = FresnelPower, y = SpecularIntensity, z = NormalMapTiling, w = NoiseIntensity
    vec4 u_NormalMapScroll;        // xy = scroll0 offset, zw = scroll1 offset
    vec4 u_NormalMapSpeed;         // x = speed0, y = speed1, z/w = unused
    vec4 u_LightDirection;         // xyz = directional light dir, w = unused
    vec4 u_ScreenParams;           // x = width, y = height, z = 1/width, w = 1/height
    vec4 u_DepthRefractionParams;  // x = depthSoftening, y = refrDistortion, z = refrHeightFactor, w = unused
    vec4 u_RefractionColor;        // rgb = tint color, w = unused
    vec4 u_FoamParams;             // x = heightStart, y = fadeDistance, z = tiling, w = brightness
    vec4 u_FoamParams2;            // x = angleExponent, y = shorelinePower, z = sssIntensity, w = unused
    vec4 u_SSSColor;               // rgb = subsurface color, w = unused
    vec4 u_SSRParams;              // x = maxSteps (0=disabled), y = stepSize, z = maxDistance, w = thickness
    vec4 u_TessParams;             // x = tessellationFactor, y = minTessDist, z = maxTessDist, w = unused
};

// Environment map for reflection (same slot as PBR shaders)
layout(binding = 9) uniform samplerCube u_EnvironmentMap;

// Scrolling normal maps and noise texture
layout(binding = 36) uniform sampler2D u_NormalMap0;
layout(binding = 37) uniform sampler2D u_NormalMap1;
layout(binding = 38) uniform sampler2D u_NoiseMap;

// Depth and refraction textures (bound by WaterRenderPass)
layout(binding = 39) uniform sampler2D u_SceneDepth;
layout(binding = 40) uniform sampler2D u_RefractionTexture;
layout(binding = 41) uniform sampler2D u_FoamTexture;

// Scene view-space normals for SSR (bound by WaterRenderPass)
layout(binding = 22) uniform sampler2D u_SceneNormals;

// Linearize a depth buffer value to view-space depth
float linearizeDepth(float d, float nearPlane, float farPlane)
{
    return nearPlane * farPlane / (farPlane - d * (farPlane - nearPlane));
}

// Procedural hash-based noise for water detail (no texture needed)
float hash2D(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// Smooth 2D value noise
float valueNoise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f); // smoothstep interpolation

    float a = hash2D(i);
    float b = hash2D(i + vec2(1.0, 0.0));
    float c = hash2D(i + vec2(0.0, 1.0));
    float d = hash2D(i + vec2(1.0, 1.0));

    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// Procedural normal from value noise (central differences)
vec3 proceduralNormal(vec2 uv, float strength)
{
    float eps = 0.02;
    float h0 = valueNoise(uv);
    float hx = valueNoise(uv + vec2(eps, 0.0));
    float hy = valueNoise(uv + vec2(0.0, eps));
    return normalize(vec3(-(hx - h0) * strength, 1.0, -(hy - h0) * strength));
}

// FBM noise for foam and detail (3 octaves)
float fbmNoise(vec2 p)
{
    float v = 0.0;
    float a = 0.5;
    mat2 rot = mat2(0.8, 0.6, -0.6, 0.8);
    for (int i = 0; i < 3; i++)
    {
        v += a * valueNoise(p);
        p = rot * p * 2.0;
        a *= 0.5;
    }
    return v;
}

// =============================================================================
// Screen Space Reflections (SSR)
// Ray-marches in view space, samples scene color at hit point
// =============================================================================

// Reconstruct view-space position from screen UV and depth
vec3 viewPosFromDepth(vec2 uv, float depth)
{
    // NDC: [-1, 1]
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    // Inverse projection to view space
    mat4 invProj = inverse(u_Projection);
    vec4 viewPos = invProj * ndc;
    return viewPos.xyz / viewPos.w;
}

// Project view-space position to screen UV
vec2 projectToScreen(vec3 viewPos)
{
    vec4 clipPos = u_Projection * vec4(viewPos, 1.0);
    vec2 ndc = clipPos.xy / clipPos.w;
    return ndc * 0.5 + 0.5;
}

// SSR ray march: returns vec4(color.rgb, confidence)
vec4 screenSpaceReflection(vec3 worldPos, vec3 reflectDirWorld)
{
    float maxSteps = u_SSRParams.x;
    float stepSize = u_SSRParams.y;
    float maxDistance = u_SSRParams.z;
    float thickness = u_SSRParams.w;

    // Convert to view space
    vec3 viewPos = (u_View * vec4(worldPos, 1.0)).xyz;
    vec3 viewReflect = normalize(mat3(u_View) * reflectDirWorld);

    // March through screen space
    vec3 rayPos = viewPos;
    float totalDistance = 0.0;
    int steps = int(maxSteps);

    for (int i = 0; i < steps; i++)
    {
        rayPos += viewReflect * stepSize;
        totalDistance += stepSize;

        if (totalDistance > maxDistance)
        {
            break;
        }

        // Project ray position to screen
        vec2 hitUV = projectToScreen(rayPos);

        // Off-screen check
        if (hitUV.x < 0.0 || hitUV.x > 1.0 || hitUV.y < 0.0 || hitUV.y > 1.0)
        {
            break;
        }

        // Sample scene depth at this screen position
        float sceneDepthSample = texture(u_SceneDepth, hitUV).r;
        vec3 sceneViewPos = viewPosFromDepth(hitUV, sceneDepthSample);

        // Check if ray is behind scene geometry
        float depthDelta = rayPos.z - sceneViewPos.z;

        if (depthDelta > 0.0 && depthDelta < thickness)
        {
            // Hit! Sample scene color
            vec3 hitColor = texture(u_RefractionTexture, hitUV).rgb;

            // Compute confidence: fade at screen edges, fade with distance
            float edgeFade = 1.0;
            edgeFade *= smoothstep(0.0, 0.05, hitUV.x) * smoothstep(1.0, 0.95, hitUV.x);
            edgeFade *= smoothstep(0.0, 0.05, hitUV.y) * smoothstep(1.0, 0.95, hitUV.y);

            float distFade = 1.0 - clamp(totalDistance / maxDistance, 0.0, 1.0);
            float stepFade = 1.0 - clamp(float(i) / float(steps), 0.0, 1.0);

            float confidence = edgeFade * distFade * stepFade;
            return vec4(hitColor, confidence);
        }

        // Adaptive step size: increase as we go further
        stepSize *= 1.02;
    }

    return vec4(0.0); // No hit: zero confidence
}

void main()
{
    vec3 gerstnerNormal = normalize(v_Normal);
    vec3 viewDir = normalize(v_ViewDir);

    // --- Normal Detail ---
    float tiling = u_VisualParams.z; // NormalMapTiling
    float time = u_WaveParams.x * u_WaveParams.y;

    // Build TBN matrix from vertex outputs
    vec3 T = normalize(v_Tangent);
    vec3 B = normalize(v_Bitangent);
    vec3 N = gerstnerNormal;
    mat3 TBN = mat3(T, B, N);

    // Check if normal map textures are actually bound (non-black check)
    // When unbound, OpenGL returns (0,0,0,0) → (0*2-1) = (-1,-1,-1) = BAD
    vec2 uv0 = v_WorldPos.xz * tiling + u_NormalMapScroll.xy;
    vec2 uv1 = v_WorldPos.xz * tiling * 0.7 + u_NormalMapScroll.zw;

    vec4 nm0Sample = texture(u_NormalMap0, uv0);
    vec4 nm1Sample = texture(u_NormalMap1, uv1);

    // Detect unbound textures: if ALL channels are exactly 0, use procedural fallback
    bool hasNormalMap0 = (nm0Sample.r + nm0Sample.g + nm0Sample.b) > 0.001;
    bool hasNormalMap1 = (nm1Sample.r + nm1Sample.g + nm1Sample.b) > 0.001;

    vec3 n0, n1;
    if (hasNormalMap0)
    {
        n0 = nm0Sample.rgb * 2.0 - 1.0;
    }
    else
    {
        // Procedural scrolling normal detail (2 octaves at different scales)
        n0 = proceduralNormal(uv0 * 8.0, 1.5);
    }

    if (hasNormalMap1)
    {
        n1 = nm1Sample.rgb * 2.0 - 1.0;
    }
    else
    {
        n1 = proceduralNormal(uv1 * 12.0, 1.2);
    }

    // Blend the two normal maps in tangent space
    vec3 blendedTangentNormal = normalize(n0 + n1);
    vec3 normalMapWorld = normalize(TBN * blendedTangentNormal);
    // Blend strength: stronger at close range for micro-detail
    vec3 normal = normalize(mix(gerstnerNormal, normalMapWorld, 0.6));

    // --- Screen-space UV ---
    vec2 screenUV = gl_FragCoord.xy * u_ScreenParams.zw;

    // --- Depth Softening ---
    float nearPlane = u_Projection[3][2] / (u_Projection[2][2] - 1.0);
    float farPlane  = u_Projection[3][2] / (u_Projection[2][2] + 1.0);

    float sceneDepthRaw = texture(u_SceneDepth, screenUV).r;
    float sceneDepthLinear = linearizeDepth(sceneDepthRaw, nearPlane, farPlane);
    float waterDepthLinear = linearizeDepth(gl_FragCoord.z, nearPlane, farPlane);
    float depthDifference = max(sceneDepthLinear - waterDepthLinear, 0.0);

    float depthSoftening = u_DepthRefractionParams.x;
    float depthFade = smoothstep(0.0, depthSoftening, depthDifference);

    // --- Refraction ---
    float refrDistortion = u_DepthRefractionParams.y;
    float refrHeightFactor = u_DepthRefractionParams.z;

    vec2 refractionOffset = normal.xz * refrDistortion * (1.0 + v_WaveHeight * refrHeightFactor);
    vec2 refractionUV = clamp(screenUV + refractionOffset, vec2(0.001), vec2(0.999));
    vec3 refractionSample = texture(u_RefractionTexture, refractionUV).rgb;

    vec3 refrTint = u_RefractionColor.rgb;
    float refrDepthTint = exp(-depthDifference * 0.5);
    vec3 refractedColor = refractionSample * mix(vec3(1.0), refrTint, 1.0 - refrDepthTint);

    // --- Fresnel ---
    float NdotV = max(dot(normal, viewDir), 0.0);
    float fresnelPower = u_VisualParams.x;
    float reflectivity = u_WaterDeepColor.a;
    // Schlick-style Fresnel with artist power control
    float fresnelFactor = reflectivity + (1.0 - reflectivity) * pow(1.0 - NdotV, fresnelPower);

    // Reflection: SSR with cubemap fallback
    vec3 reflectDir = reflect(-viewDir, normal);
    vec3 cubemapReflection = texture(u_EnvironmentMap, reflectDir).rgb;
    vec3 reflectionColor = cubemapReflection;

    // Screen Space Reflections (when enabled: u_SSRParams.x > 0)
    if (u_SSRParams.x > 0.0)
    {
        vec4 ssrResult = screenSpaceReflection(v_WorldPos, reflectDir);
        // Blend SSR with cubemap fallback based on confidence
        reflectionColor = mix(cubemapReflection, ssrResult.rgb, ssrResult.a);
    }

    // Blend shallow and deep water colors based on view angle + depth
    vec3 shallowColor = u_WaterColor.rgb;
    vec3 deepColor = u_WaterDeepColor.rgb;
    float depthColorBlend = 1.0 - exp(-depthDifference * 0.3);
    float viewDepthBlend = max(depthColorBlend, 1.0 - NdotV); // deep color at grazing angles too
    vec3 waterBaseColor = mix(shallowColor, deepColor, viewDepthBlend);

    // Blend refraction (visible in shallow areas) with water body color (in deep areas)
    // Clamp minimum blend so water body color always dominates — prevents
    // the editor grid / underlying geometry "bleeding" through the surface.
    float minWaterOpacity = 0.9; // Water body color always at least 90%
    float waterBlend = max(clamp(depthColorBlend, 0.0, 1.0), minWaterOpacity);
    vec3 waterColor = mix(refractedColor, waterBaseColor, waterBlend);

    // Combine reflection with water color via Fresnel
    vec3 finalColor = mix(waterColor, reflectionColor, fresnelFactor);

    // --- Ambient ocean contribution ---
    // Provides a minimum brightness so the water is never pitch black,
    // even without strong reflections or cubemap.  Simulates sky light
    // scattering through the upper water column.
    vec3 ambientOcean = shallowColor * 0.15;
    finalColor += ambientOcean;

    // --- Specular with noise modulation ---
    vec3 lightDir = normalize(u_LightDirection.xyz);
    if (dot(lightDir, lightDir) < 0.001)
    {
        lightDir = normalize(vec3(0.5, 1.0, 0.3));
    }
    vec3 halfVec = normalize(viewDir + lightDir);
    float specAngle = max(dot(normal, halfVec), 0.0);

    // Two specular lobes: tight sun disk + broader sparkle
    float noiseIntensity = u_VisualParams.w;
    float sparkleNoise = 1.0;
    if (noiseIntensity > 0.0)
    {
        // Use procedural noise if no noise texture is bound
        vec4 noiseSample = texture(u_NoiseMap, v_WorldPos.xz * 0.5 + u_NormalMapScroll.xy * 0.3);
        bool hasNoiseMap = (noiseSample.r + noiseSample.g + noiseSample.b) > 0.001;
        float noiseVal;
        if (hasNoiseMap)
        {
            noiseVal = noiseSample.r;
        }
        else
        {
            // Procedural sparkle noise at 3 different frequencies
            float n1p = valueNoise(v_WorldPos.xz * 3.0 + u_NormalMapScroll.xy * 5.0);
            float n2p = valueNoise(v_WorldPos.xz * 7.0 - u_NormalMapScroll.zw * 3.0);
            float n3p = valueNoise(v_WorldPos.xz * 13.0 + time * 0.5);
            noiseVal = n1p * n2p * n3p;
            noiseVal = smoothstep(0.02, 0.15, noiseVal); // Threshold for sparkle dots
        }
        sparkleNoise = mix(1.0, noiseVal, noiseIntensity);
    }

    float specIntensity = u_VisualParams.y;
    // Tight sun specular (always present)
    float sunSpec = pow(specAngle, 256.0) * specIntensity * 2.0;
    // Broader sparkle (noise-modulated) — wide lobe gives wet glint across the surface
    float sparkleSpec = pow(specAngle, 16.0) * specIntensity * 0.35 * sparkleNoise;
    finalColor += vec3(sunSpec + sparkleSpec);

    // --- Subsurface Scattering (SSS) ---
    float sssIntensity = u_FoamParams2.z;
    if (sssIntensity > 0.0)
    {
        float sssFactor = pow(clamp(dot(viewDir, -lightDir), 0.0, 1.0), 4.0);
        sssFactor *= clamp(v_WaveHeight * 0.5 + 0.5, 0.0, 1.0);
        // SSS only on thin crests facing the light
        float sssThickness = clamp(1.0 - abs(dot(gerstnerNormal, vec3(0.0, 1.0, 0.0))), 0.0, 1.0);
        sssFactor *= (0.5 + 0.5 * sssThickness);
        finalColor += u_SSSColor.rgb * sssFactor * sssIntensity;
    }

    // --- Foam ---
    float foamHeightStart = u_FoamParams.x;
    float foamFadeDistance = u_FoamParams.y;
    float foamTiling = u_FoamParams.z;
    float foamBrightness = u_FoamParams.w;
    float foamAngleExponent = u_FoamParams2.x;
    float shorelineFoamPower = u_FoamParams2.y;

    // Height-based foam (wave crests)
    float heightFoam = smoothstep(foamHeightStart, foamHeightStart + foamFadeDistance, v_WaveHeight);

    // Angle-based foam (steep wave faces)
    float steepness = 1.0 - max(dot(gerstnerNormal, vec3(0.0, 1.0, 0.0)), 0.0);
    float angleFoam = pow(steepness, foamAngleExponent);

    // Shoreline foam (depth-based)
    float safeSoftening = max(depthSoftening, 0.001);
    float shorelineFoam = pow(1.0 - clamp(depthDifference / safeSoftening, 0.0, 1.0), shorelineFoamPower);

    // --- Large-scale spatial noise gate ---
    // Prevents foam from appearing on EVERY wave crest.
    // Very low-frequency noise so only sparse, random patches of
    // crests get foam — eliminates the grid/checkerboard pattern.
    float foamGateNoise = fbmNoise(v_WorldPos.xz * 0.03 + vec2(5.3, 11.7));
    float foamGate = smoothstep(0.55, 0.85, foamGateNoise);  // ~15-20% of area gets foam — rarer, more clustered
    heightFoam *= foamGate;
    angleFoam  *= foamGate;

    float foam = max(max(heightFoam, angleFoam), shorelineFoam);

    // Modulate foam with texture OR procedural noise
    vec2 foamUV = v_WorldPos.xz * foamTiling + u_NormalMapScroll.xy * 0.2;
    vec4 foamSample = texture(u_FoamTexture, foamUV);
    bool hasFoamTexture = (foamSample.r + foamSample.g + foamSample.b) > 0.001;

    if (hasFoamTexture)
    {
        foam *= foamSample.r;
    }
    else
    {
        // Procedural foam pattern: multi-octave smooth noise
        // Two scales blended to break up regularity
        float foamNoise1 = fbmNoise(foamUV * 1.5);
        float foamNoise2 = fbmNoise(foamUV * 3.7 + vec2(17.3, 31.7));
        float foamNoise = foamNoise1 * 0.6 + foamNoise2 * 0.4;
        // Smooth, wide ramp instead of hard threshold — avoids speckle
        float foamPattern = smoothstep(0.25, 0.65, foamNoise);
        foam *= foamPattern;
    }

    finalColor = mix(finalColor, vec3(foamBrightness), clamp(foam, 0.0, 1.0));

    // --- Final alpha ---
    // Deep ocean water is fully opaque. Only edges/shoreline might
    // soften via depthFade, but we keep a near-opaque floor.
    float transparency = 1.0;

    o_Color = vec4(finalColor, transparency);
    o_EntityID = u_EntityID;
    o_ViewNormal = octEncode(normalize(mat3(u_View) * normal));

    // Camera-motion velocity from the interpolated (displaced) world pos.
    vec4 clipCurr = u_ViewProjection     * vec4(v_WorldPos, 1.0);
    vec4 clipPrev = u_PrevViewProjection * vec4(v_WorldPos, 1.0);
    vec2 ndcCurr = clipCurr.xy / clipCurr.w;
    vec2 ndcPrev = clipPrev.xy / clipPrev.w;
    o_Velocity = (ndcCurr - ndcPrev) * 0.5;
}

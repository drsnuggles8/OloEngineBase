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
    // Previous-frame VP for scene FB RT3 velocity. Wave displacement itself is
    // reprojected by re-evaluating sumGerstnerWaves() at `u_NormalMapSpeed.z`
    // (= prev animation time) in VS/TES so the motion vector captures on-
    // surface wave motion, not just camera and rigid motion.
    mat4 u_PrevViewProjection;
    vec3 u_RenderOrigin; // camera-relative render origin (issue #429)
    float _padding1;
};

// Model UBO (binding 3)
#include "include/InstanceBlock_Vertex.glsl"

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
    vec4 u_FFTParams;              // x = useFFT (0/1), y = 1/patchSize, z = heightScale, w = horizontalScale
};

#include "include/WaterCommon.glsl"

// FFT ocean cascade textures (WATER_FUTURE_IMPROVEMENTS.md §1). Sampled when
// u_FFTParams.x > 0.5 instead of summing Gerstner waves analytically.
layout(binding = 50) uniform sampler2D u_FFTDisplacement; // rgb = (dx, h, dz), a = foam
layout(binding = 51) uniform sampler2D u_FFTDerivatives;  // rgb = normal, a = jacobian

layout(location = 0) out vec3 v_WorldPos;
layout(location = 1) out vec3 v_Normal;
layout(location = 2) out vec2 v_TexCoord;
layout(location = 3) out vec3 v_ViewDir;
layout(location = 4) out vec3 v_Tangent;
layout(location = 5) out vec3 v_Bitangent;
layout(location = 6) out float v_WaveHeight;
// Previous-frame world position (wave + model reprojection) for RT3 velocity.
layout(location = 7) out vec3 v_PrevWorldPos;

void main()
{
    OLO_INSTANCE_FORWARD();
    vec4 worldPos = u_Model * vec4(a_Position, 1.0);
    vec4 worldPosPrev = u_PrevModel * vec4(a_Position, 1.0);
    // Camera-relative (issue #429): u_Model is render-relative, so add the render
    // origin back for the world-anchored wave phase / FFT field sampling. The
    // displaced position stays relative (Gerstner sums are shifted back by the
    // origin) so gl_Position uses the relative view-projection.
    vec3 worldPosAbs = worldPos.xyz + u_RenderOrigin;
    vec3 worldPosPrevAbs = worldPosPrev.xyz + u_RenderOrigin;

    float time = u_WaveParams.x * u_WaveParams.y;                   // Time * WaveSpeed
    float prevTime = u_NormalMapSpeed.z * u_WaveParams.y;            // PrevTime * WaveSpeed
    float amplitude = u_WaveParams.z;
    float frequency = u_WaveParams.w;

    // When tessellation is active, vertex shader passes through
    // (displacement happens in TES instead)
    if (u_TessParams.x > 0.0)
    {
        v_WorldPos = worldPos.xyz;
        v_PrevWorldPos = worldPosPrev.xyz; // TES will re-displace
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
    vec3 displacedPos;
    if (u_FFTParams.x > 0.5)
    {
        // FFT ocean: sample the spectral displacement field (tiles by patch size).
        vec2 fftUV = worldPosAbs.xz * u_FFTParams.y; // world-anchored (issue #429)
        vec4 disp = textureLod(u_FFTDisplacement, fftUV, 0.0);
        displacedPos = worldPos.xyz + vec3(disp.x * u_FFTParams.w, disp.y * u_FFTParams.z, disp.z * u_FFTParams.w);
        displacedNormal = normalize(textureLod(u_FFTDerivatives, fftUV, 0.0).xyz);
        v_PrevWorldPos = displacedPos; // FFT field has no prev-frame copy → no wave reprojection
    }
    else
    {
        // World-anchored phase (issue #429): evaluate at the absolute world
        // position, then shift the returned displaced position back to relative
        // space (sumGerstnerWaves returns position + displacement).
        displacedPos = sumGerstnerWaves(
            worldPosAbs, time,
            u_WaveDir0, u_WaveDir1,
            frequency, amplitude,
            displacedNormal
        ) - u_RenderOrigin;

        // Prev-frame displaced position — same Gerstner sum evaluated at prev time
        // through the prev model transform so the motion vector captures wave sway.
        vec3 _prevNormalUnused;
        vec3 displacedPosPrev = sumGerstnerWaves(
            worldPosPrevAbs, prevTime,
            u_WaveDir0, u_WaveDir1,
            frequency, amplitude,
            _prevNormalUnused
        ) - u_RenderOrigin;
        v_PrevWorldPos = displacedPosPrev;
    }

    // Normalized wave height for foam/SSS. Gerstner divides by amplitude to get a
    // ~[-1,1] range; FFT height is already in metres (its own amplitude), so pass
    // it through raw — foamHeightStart then reads as a metre threshold.
    float maxAmplitude = max(amplitude, 0.001);
    v_WaveHeight = (u_FFTParams.x > 0.5) ? (displacedPos.y - worldPos.y)
                                         : (displacedPos.y - worldPos.y) / maxAmplitude;

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
    vec3 u_RenderOrigin; // camera-relative render origin (issue #429)
    float _padding1;
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
    vec4 u_FFTParams;
};

layout(location = 0) in vec3 v_WorldPos[];
layout(location = 1) in vec3 v_Normal[];
layout(location = 2) in vec2 v_TexCoord[];
layout(location = 3) in vec3 v_ViewDir[];
layout(location = 4) in vec3 v_Tangent[];
layout(location = 5) in vec3 v_Bitangent[];
layout(location = 6) in float v_WaveHeight[];
layout(location = 7) in vec3 v_PrevWorldPos[];

layout(location = 0) out vec3 tc_WorldPos[];
layout(location = 1) out vec3 tc_Normal[];
layout(location = 2) out vec2 tc_TexCoord[];
layout(location = 3) out vec3 tc_PrevWorldPos[];

float calcTessLevel(vec3 p0, vec3 p1)
{
    // u_TessParams.x is the near-camera subdivision factor. When tessellation is
    // disabled the C++ side passes 0 here — but a DRAWN patch needs a tess level
    // of at least 1; a level below 1 (and especially 0) discards the patch
    // entirely (GL 4.6 §11.2.2). The old mix(maxFactor, 1.0, t) therefore handed
    // near patches (t≈0) a level of ~0 whenever tessellation was off, silently
    // culling all the water close to the camera and leaving a see-through hole to
    // the seafloor. Clamp the factor (and the result) to >= 1 so the base grid is
    // always rasterised; "disabled" just means no extra subdivision.
    float maxFactor = max(u_TessParams.x, 1.0);
    float minDist = u_TessParams.y;
    float maxDist = u_TessParams.z;

    vec3 mid = (p0 + p1) * 0.5;
    float dist = distance(u_CameraPosition, mid);

    // Linear falloff: close = maxFactor, far = 1.0
    float t = clamp((dist - minDist) / max(maxDist - minDist, 0.001), 0.0, 1.0);
    return max(mix(maxFactor, 1.0, t), 1.0);
}

// Conservative upper bound on per-axis Gerstner displacement for the current
// wave parameters. Used as a culling margin: a patch's corners are inflated by
// this distance before the frustum test so waves at the edges of off-screen
// patches don't pop into view. Derived from the wave bookkeeping in
// `sumGerstnerWaves` (WaterCommon.glsl): per wave the displacement components
// are bounded by `steepness * wavelength / 2π`, summed with the per-octave
// amplitude weights (0.55, 0.55, 0.5, 0.4, 0.3, 0.22, 0.15, 0.1 = 2.77).
// Detail octaves use the avg-wavelength × avg-steepness fall-back; bounding
// each by the largest octave's product over-estimates, which is the safe
// direction for a culling margin. A 1.5x safety factor absorbs phase / domain-
// warp interactions that the per-axis split-then-sum derivation ignores.
float computeMaxWaveDisplacement()
{
    const float TWO_PI = 6.28318530;
    float freq = max(u_WaveParams.w, 0.01);
    float amp = u_WaveParams.z;

    float wl0 = max(u_WaveDir0.w, 0.1) / freq;
    float wl1 = max(u_WaveDir1.w, 0.1) / freq;
    float a0 = u_WaveDir0.z * wl0 / TWO_PI;
    float a1 = u_WaveDir1.z * wl1 / TWO_PI;

    float avgWL = (wl0 + wl1) * 0.5;
    float avgSt = (u_WaveDir0.z + u_WaveDir1.z) * 0.5;
    float maxOctaveA = avgSt * avgWL / TWO_PI;
    // Detail-octave amp-weight sum = 0.5+0.4+0.3+0.22+0.15+0.1
    float octaveSum = maxOctaveA * 1.67;

    return amp * (a0 * 0.55 + a1 * 0.55 + octaveSum) * 1.5;
}

// True when the patch (corners p0/p1/p2) is entirely outside the view frustum
// once each corner is grown by `margin` along every axis. Uses the standard
// Gribb-Hartmann plane extraction from the view-projection matrix; planes are
// kept un-normalised and the sphere-vs-plane threshold is scaled by
// `length(plane.xyz)` so the test stays correct regardless of perspective
// asymmetry.
bool isPatchOutsideFrustum(vec3 p0, vec3 p1, vec3 p2, float margin)
{
    mat4 vp = u_ViewProjection;
    // GLSL is column-major: vp[col][row]. Row i = vec4(vp[0][i] .. vp[3][i]).
    vec4 row0 = vec4(vp[0][0], vp[1][0], vp[2][0], vp[3][0]);
    vec4 row1 = vec4(vp[0][1], vp[1][1], vp[2][1], vp[3][1]);
    vec4 row2 = vec4(vp[0][2], vp[1][2], vp[2][2], vp[3][2]);
    vec4 row3 = vec4(vp[0][3], vp[1][3], vp[2][3], vp[3][3]);

    vec4 planes[6];
    planes[0] = row3 + row0; // left
    planes[1] = row3 - row0; // right
    planes[2] = row3 + row1; // bottom
    planes[3] = row3 - row1; // top
    planes[4] = row3 + row2; // near (GL [-1,1] clip-space depth)
    planes[5] = row3 - row2; // far

    for (int i = 0; i < 6; ++i)
    {
        vec4 plane = planes[i];
        float normFactor = length(plane.xyz);
        // A patch is fully outside this plane when every inflated corner's
        // signed distance is more negative than `margin * normFactor`.
        float d0 = dot(plane.xyz, p0) + plane.w;
        float d1 = dot(plane.xyz, p1) + plane.w;
        float d2 = dot(plane.xyz, p2) + plane.w;
        float threshold = -margin * normFactor;
        if (d0 < threshold && d1 < threshold && d2 < threshold)
            return true;
    }
    return false;
}

void main()
{
    tc_WorldPos[gl_InvocationID] = v_WorldPos[gl_InvocationID];
    tc_Normal[gl_InvocationID] = v_Normal[gl_InvocationID];
    tc_TexCoord[gl_InvocationID] = v_TexCoord[gl_InvocationID];
    tc_PrevWorldPos[gl_InvocationID] = v_PrevWorldPos[gl_InvocationID];

    if (gl_InvocationID == 0)
    {
        // Skip patches whose Gerstner-displaced bound lies entirely outside
        // the view frustum. u_TessParams.w == 0 keeps the legacy "always
        // tessellate" behaviour so the cull can be disabled from the C++
        // side without touching the shader.
        bool culled = false;
        if (u_TessParams.w > 0.5)
        {
            float margin = computeMaxWaveDisplacement();
            culled = isPatchOutsideFrustum(v_WorldPos[0], v_WorldPos[1], v_WorldPos[2], margin);
        }

        if (culled)
        {
            // Setting any TessLevelOuter to 0 discards the patch (GL 4.6 §11.2.2),
            // saving the TES invocations and rasterizer setup for off-screen patches.
            gl_TessLevelOuter[0] = 0.0;
            gl_TessLevelOuter[1] = 0.0;
            gl_TessLevelOuter[2] = 0.0;
            gl_TessLevelInner[0] = 0.0;
        }
        else
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
    vec3 u_RenderOrigin; // camera-relative render origin (issue #429)
    float _padding1;
};

#include "include/InstanceBlock_Single.glsl"

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
    vec4 u_FFTParams;
};

#include "include/WaterCommon.glsl"

// FFT ocean cascade textures (WATER_FUTURE_IMPROVEMENTS.md §1).
layout(binding = 50) uniform sampler2D u_FFTDisplacement; // rgb = (dx, h, dz), a = foam
layout(binding = 51) uniform sampler2D u_FFTDerivatives;  // rgb = normal, a = jacobian

layout(location = 0) in vec3 tc_WorldPos[];
layout(location = 1) in vec3 tc_Normal[];
layout(location = 2) in vec2 tc_TexCoord[];
layout(location = 3) in vec3 tc_PrevWorldPos[];

layout(location = 0) out vec3 v_WorldPos;
layout(location = 1) out vec3 v_Normal;
layout(location = 2) out vec2 v_TexCoord;
layout(location = 3) out vec3 v_ViewDir;
layout(location = 4) out vec3 v_Tangent;
layout(location = 5) out vec3 v_Bitangent;
layout(location = 6) out float v_WaveHeight;
layout(location = 7) out vec3 v_PrevWorldPos;

void main()
{
    // Barycentric interpolation
    vec3 pos = gl_TessCoord.x * tc_WorldPos[0]
             + gl_TessCoord.y * tc_WorldPos[1]
             + gl_TessCoord.z * tc_WorldPos[2];
    vec3 posPrev = gl_TessCoord.x * tc_PrevWorldPos[0]
                 + gl_TessCoord.y * tc_PrevWorldPos[1]
                 + gl_TessCoord.z * tc_PrevWorldPos[2];
    vec2 uv = gl_TessCoord.x * tc_TexCoord[0]
            + gl_TessCoord.y * tc_TexCoord[1]
            + gl_TessCoord.z * tc_TexCoord[2];

    // Camera-relative (issue #429): pos is render-relative; add the origin back
    // for the world-anchored wave phase / FFT sampling. Displaced position stays
    // relative (Gerstner sums shifted back by origin).
    vec3 posAbs = pos + u_RenderOrigin;
    vec3 posPrevAbs = posPrev + u_RenderOrigin;

    // Apply wave displacement (FFT ocean or analytic Gerstner)
    float time = u_WaveParams.x * u_WaveParams.y;
    float prevTime = u_NormalMapSpeed.z * u_WaveParams.y;
    float amplitude = u_WaveParams.z;
    float frequency = u_WaveParams.w;

    vec3 displacedNormal;
    vec3 displacedPos;
    if (u_FFTParams.x > 0.5)
    {
        vec2 fftUV = posAbs.xz * u_FFTParams.y; // world-anchored (issue #429)
        vec4 disp = textureLod(u_FFTDisplacement, fftUV, 0.0);
        displacedPos = pos + vec3(disp.x * u_FFTParams.w, disp.y * u_FFTParams.z, disp.z * u_FFTParams.w);
        displacedNormal = normalize(textureLod(u_FFTDerivatives, fftUV, 0.0).xyz);
        v_PrevWorldPos = displacedPos; // FFT field has no prev-frame copy
    }
    else
    {
        displacedPos = sumGerstnerWaves(
            posAbs, time,
            u_WaveDir0, u_WaveDir1,
            frequency, amplitude,
            displacedNormal
        ) - u_RenderOrigin; // world-anchored phase, relative result (issue #429)

        // Prev-frame displacement for velocity reprojection
        vec3 _prevNormalUnused;
        vec3 displacedPosPrev = sumGerstnerWaves(
            posPrevAbs, prevTime,
            u_WaveDir0, u_WaveDir1,
            frequency, amplitude,
            _prevNormalUnused
        ) - u_RenderOrigin;
        v_PrevWorldPos = displacedPosPrev;
    }

    float maxAmplitude = max(amplitude, 0.001);
    v_WaveHeight = (u_FFTParams.x > 0.5) ? (displacedPos.y - pos.y)
                                         : (displacedPos.y - pos.y) / maxAmplitude;

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
layout(location = 7) in vec3 v_PrevWorldPos;

// MRT outputs matching SceneRenderPass format
layout(location = 0) out vec4 o_Color;
layout(location = 1) out int o_EntityID;
layout(location = 2) out vec2 o_ViewNormal;
// Scene FB RT3 velocity. Captures camera, per-object motion, AND the
// per-fragment wave reprojection: v_PrevWorldPos carries the Gerstner sum
// re-evaluated at `u_NormalMapSpeed.z * u_WaveParams.y` (prev time * speed)
// in the vertex / tessellation stage, so TAA resolves moving waves cleanly.
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
    vec3 u_RenderOrigin; // camera-relative render origin (issue #429)
    float _padding1;
};

// Instance SSBO (binding 15). Water is single-instance — the tess_eval
// stage uses InstanceBlock_Single (no v_InstanceIndex output), so the
// fragment stage must match it rather than declaring `flat in int
// v_InstanceIndex` with no producer (link error).
#include "include/InstanceBlock_Single.glsl"

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
    vec4 u_NormalMapSpeed;         // x = speed0, y = speed1, z = PrevTime, w = unused
    vec4 u_LightDirection;         // xyz = directional light dir, w = unused
    vec4 u_ScreenParams;           // x = width, y = height, z = 1/width, w = 1/height
    vec4 u_DepthRefractionParams;  // x = depthSoftening, y = refrDistortion, z = refrHeightFactor, w = unused
    vec4 u_RefractionColor;        // rgb = tint color, w = unused
    vec4 u_FoamParams;             // x = heightStart, y = fadeDistance, z = tiling, w = brightness
    vec4 u_FoamParams2;            // x = angleExponent, y = shorelinePower, z = sssIntensity, w = unused
    vec4 u_SSSColor;               // rgb = subsurface color, w = unused
    vec4 u_SSRParams;              // x = maxSteps (0=disabled), y = stepSize, z = maxDistance, w = thickness
    vec4 u_TessParams;             // x = tessellationFactor, y = minTessDist, z = maxTessDist, w = unused
    vec4 u_FFTParams;              // x = useFFT (0/1), y = 1/patchSize, z = heightScale, w = horizontalScale
};

// Environment map for reflection (same slot as PBR shaders)
layout(binding = 9) uniform samplerCube u_EnvironmentMap;

// FFT ocean displacement (binding 50): a-channel carries the Jacobian-based foam.
layout(binding = 50) uniform sampler2D u_FFTDisplacement;

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

// Planar reflection — the opaque scene re-rendered from a mirrored, oblique-
// clipped camera by PlanarReflectionRenderPass. u_PlanarReflectionVP projects a
// world position into that target; Params.x gates the whole feature so a stale
// texture is never sampled when the reflection pass is disabled.
layout(std140, binding = 43) uniform PlanarReflectionParams
{
    mat4 u_PlanarReflectionVP;   // world -> mirrored reflection clip space
    vec4 u_PlanarReflectionData; // x = enabled (0/1), y = intensity, z = distortion, w = unused
};
layout(binding = 52) uniform sampler2D u_PlanarReflectionTexture;

// Sample the planar reflection at this surface point, perturbing the projected
// UV by the surface normal so ripples break up the mirror. Returns rgb in .rgb
// and a [0,1] confidence in .a (0 when disabled or the sample falls off-screen).
vec4 samplePlanarReflection(vec3 worldPos, vec3 normal)
{
    if (u_PlanarReflectionData.x < 0.5)
        return vec4(0.0);

    vec4 clip = u_PlanarReflectionVP * vec4(worldPos, 1.0);
    if (clip.w <= 0.0)
        return vec4(0.0);

    vec2 reflUV = (clip.xy / clip.w) * 0.5 + 0.5;
    // The reflection target is rendered with a mirrored camera, so its X is
    // already flipped relative to the main view — no extra flip needed.
    reflUV += normal.xz * u_PlanarReflectionData.z;

    // Fade out toward the screen edge so the mirror doesn't smear where the
    // reflected geometry runs off the reflection viewport.
    vec2 edge = smoothstep(vec2(0.0), vec2(0.06), reflUV) *
                (1.0 - smoothstep(vec2(0.94), vec2(1.0), reflUV));
    float confidence = edge.x * edge.y;
    if (confidence <= 0.0)
        return vec4(0.0);

    vec3 reflColor = texture(u_PlanarReflectionTexture, clamp(reflUV, vec2(0.001), vec2(0.999))).rgb;
    return vec4(reflColor, confidence * clamp(u_PlanarReflectionData.y, 0.0, 1.0));
}

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
    // Per-fragment waterline side selection (§7.2). When render-from-below is
    // enabled (u_NormalMapSpeed.w > 0.5) the water draws double-sided; keep only
    // the face whose side the camera is actually on relative to THIS fragment:
    // a fragment above the eye shows its underside, one below shows its top.
    // This avoids both the see-through holes of single-sided culling and the
    // interleaved-sheet mess of naive double-siding when the camera straddles
    // the waterline. (With render-from-below off the draw is back-culled and
    // this branch is inert.)
    if (u_NormalMapSpeed.w > 0.5)
    {
        bool cameraBelowFragment = v_WorldPos.y > u_CameraPosition.y;
        if (cameraBelowFragment == gl_FrontFacing)
            discard;
    }

    vec3 gerstnerNormal = normalize(v_Normal);
    vec3 viewDir = normalize(v_ViewDir);

    // Underside shading: kept back faces (camera below this fragment) face away
    // from the viewer, so flip the shading normal to face the camera before the
    // Fresnel / reflection / cheap-underside path below.
    if (!gl_FrontFacing)
    {
        gerstnerNormal = -gerstnerNormal;
    }

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
    vec2 uv0 = (v_WorldPos.xz + u_RenderOrigin.xz) * tiling + u_NormalMapScroll.xy;
    vec2 uv1 = (v_WorldPos.xz + u_RenderOrigin.xz) * tiling * 0.7 + u_NormalMapScroll.zw;

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

    // --- Underside (camera submerged) ---
    // Cheap, stable shading for the surface seen from below. Screen-space
    // reflection and refraction are sampled from an above-water frame of
    // reference and produce flickering garbage from underneath, so the
    // underside is just a tinted surface with a soft cubemap rim. The whole-
    // scene underwater tint is applied separately in the tone-map pass. §7.2.
    if (!gl_FrontFacing)
    {
        float NdotVu = max(dot(normal, viewDir), 0.0);
        vec3 underColor = mix(u_WaterColor.rgb, u_WaterDeepColor.rgb, 0.5);
        vec3 cubemapU = texture(u_EnvironmentMap, reflect(-viewDir, normal)).rgb;
        // Subtle rim toward the cubemap at grazing angles so it isn't flat.
        // (1-NdotVu)^4 as a multiply chain — avoids pow()'s exp2/log2 per pixel.
        float rimBase = 1.0 - NdotVu;
        float rimBase2 = rimBase * rimBase;
        float rim = rimBase2 * rimBase2;
        vec3 underFinal = mix(underColor, cubemapU, rim * 0.25);

        o_Color = vec4(underFinal, 1.0);
        o_EntityID = u_EntityID;
        o_ViewNormal = octEncode(normalize(mat3(u_View) * normal));
        vec4 clipCurrU = u_ViewProjection     * vec4(v_WorldPos,     1.0);
        vec4 clipPrevU = u_PrevViewProjection * vec4(v_PrevWorldPos, 1.0);
        o_Velocity = (clipCurrU.xy / clipCurrU.w - clipPrevU.xy / clipPrevU.w) * 0.5;
        return;
    }

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

    // View-INDEPENDENT water-column depth: reconstruct the seafloor's world
    // height under this fragment. The screen-space depthDifference collapses at
    // grazing angles (making the surface look transparent / foam flicker), so
    // both opacity and shoreline foam use this vertical depth instead. Computed
    // once here and reused below.
    //
    // `hasFloorBehind` is false when there's no opaque geometry behind the
    // surface (open ocean / sky at the far plane). There's no bottom to see in
    // that case, so the water must read as fully opaque (and grow no shoreline
    // foam) — otherwise the far-plane reconstruction yields a near-zero depth
    // and the open ocean turns transparent. This is the deep-water default.
    bool hasFloorBehind = sceneDepthRaw < 0.9999;
    vec3 floorViewPos = viewPosFromDepth(screenUV, sceneDepthRaw);
    float floorWorldY = (inverse(u_View) * vec4(floorViewPos, 1.0)).y;
    float verticalWaterDepth = max(v_WorldPos.y - floorWorldY, 0.0);

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

    // Planar (mirror) reflection takes priority where available — it is a true
    // re-render of the scene, not a screen-space / cubemap approximation. Blend
    // it over the cubemap/SSR result by its confidence (edge fade) and intensity.
    vec4 planar = samplePlanarReflection(v_WorldPos, normal);
    reflectionColor = mix(reflectionColor, planar.rgb, planar.a);

    // Blend shallow and deep water colors based on view angle + depth
    vec3 shallowColor = u_WaterColor.rgb;
    vec3 deepColor = u_WaterDeepColor.rgb;
    float depthColorBlend = 1.0 - exp(-depthDifference * 0.3);
    float viewDepthBlend = max(depthColorBlend, 1.0 - NdotV); // deep color at grazing angles too
    vec3 waterBaseColor = mix(shallowColor, deepColor, viewDepthBlend);

    // Refraction opacity from the VIEW-INDEPENDENT water-column depth, so the
    // surface reads as opaque from every angle (it never goes see-through at
    // grazing). Only genuinely shallow water (a metre or so — true shorelines,
    // against pillars) lets the bottom show; anything deeper is fully opaque
    // water body colour. exp(-d*2): ~0.86 opaque at 1 m, ~0.98 at 2 m, ~1 beyond.
    // Open ocean (no floor behind the surface) is always fully opaque.
    float waterBlend = hasFloorBehind ? (1.0 - exp(-verticalWaterDepth * 2.0)) : 1.0;
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
        vec4 noiseSample = texture(u_NoiseMap, (v_WorldPos.xz + u_RenderOrigin.xz) * 0.5 + u_NormalMapScroll.xy * 0.3);
        bool hasNoiseMap = (noiseSample.r + noiseSample.g + noiseSample.b) > 0.001;
        float noiseVal;
        if (hasNoiseMap)
        {
            noiseVal = noiseSample.r;
        }
        else
        {
            // Procedural sparkle noise at 3 different frequencies
            float n1p = valueNoise((v_WorldPos.xz + u_RenderOrigin.xz) * 3.0 + u_NormalMapScroll.xy * 5.0);
            float n2p = valueNoise((v_WorldPos.xz + u_RenderOrigin.xz) * 7.0 - u_NormalMapScroll.zw * 3.0);
            float n3p = valueNoise((v_WorldPos.xz + u_RenderOrigin.xz) * 13.0 + time * 0.5);
            noiseVal = n1p * n2p * n3p;
            noiseVal = smoothstep(0.02, 0.15, noiseVal); // Threshold for sparkle dots
        }
        sparkleNoise = mix(1.0, noiseVal, noiseIntensity);
    }

    float specIntensity = u_VisualParams.y;
    // specAngle^16 and ^256 as squaring chains (specAngle in [0,1]) — avoids two
    // pow() exp2/log2 pairs per water pixel; ^256 reuses the ^16 partial.
    float sa2 = specAngle * specAngle;
    float sa4 = sa2 * sa2;
    float sa8 = sa4 * sa4;
    float sa16 = sa8 * sa8;     // specAngle^16
    float sa256 = sa16 * sa16;  // ^32
    sa256 = sa256 * sa256;      // ^64
    sa256 = sa256 * sa256;      // ^128
    sa256 = sa256 * sa256;      // ^256
    // Tight sun specular (always present)
    float sunSpec = sa256 * specIntensity * 2.0;
    // Broader sparkle (noise-modulated) — wide lobe gives wet glint across the surface
    float sparkleSpec = sa16 * specIntensity * 0.35 * sparkleNoise;
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

    // Shoreline foam — uses the same view-INDEPENDENT water-column depth as the
    // opacity above (computed once in the depth section), so foam only appears
    // where the water is genuinely shallow (true shorelines / against pillars)
    // and never flickers with camera angle over open water. See §7.2.
    const float shorelineDepthRange = 1.5; // metres of water depth over which shore foam fades out
    float shorelineFoam = hasFloorBehind
        ? pow(1.0 - clamp(verticalWaterDepth / shorelineDepthRange, 0.0, 1.0), shorelineFoamPower)
        : 0.0; // open ocean (no floor behind) → no shoreline foam

    // --- Large-scale spatial noise gate ---
    // Prevents foam from appearing on EVERY wave crest.
    // Very low-frequency noise so only sparse, random patches of
    // crests get foam — eliminates the grid/checkerboard pattern.
    float foamGateNoise = fbmNoise((v_WorldPos.xz + u_RenderOrigin.xz) * 0.03 + vec2(5.3, 11.7));
    float foamGate = smoothstep(0.62, 0.88, foamGateNoise);  // ~10% of area gets foam — sparse, clustered whitecaps
    heightFoam *= foamGate;
    angleFoam  *= foamGate;

    float foam = max(max(heightFoam, angleFoam), shorelineFoam);

    // FFT ocean: Jacobian-based foam where the choppy surface folds (§2.1). The
    // displacement texture's alpha is saturate(1 - J): 0 on smooth water, →1 in
    // the pinched, breaking crests. This is far more physically plausible than
    // the height/angle thresholds above, so fold it in as a strong contributor.
    if (u_FFTParams.x > 0.5)
    {
        float fftFoam = textureLod(u_FFTDisplacement, (v_WorldPos.xz + u_RenderOrigin.xz) * u_FFTParams.y, 0.0).a;
        foam = max(foam, fftFoam);
    }

    // Distance fade: at grazing angles the foam patches compress toward the
    // horizon into a continuous white wash that dominates the frame. Fade foam
    // out fairly aggressively with distance so only nearby waves (where foam
    // detail is actually resolvable) keep it; far water reads as smooth tinted
    // ocean. See §7.2.
    float foamCamDist = length(u_CameraPosition - v_WorldPos);
    foam *= 1.0 - smoothstep(12.0, 45.0, foamCamDist);

    // Modulate foam with texture OR procedural noise
    vec2 foamUV = (v_WorldPos.xz + u_RenderOrigin.xz) * foamTiling + u_NormalMapScroll.xy * 0.2;
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

    // Camera + wave-reprojection velocity. v_PrevWorldPos is the Gerstner
    // displacement re-evaluated at prev time (packed into u_NormalMapSpeed.z)
    // through u_PrevModel, so on-surface wave motion is captured correctly.
    vec4 clipCurr = u_ViewProjection     * vec4(v_WorldPos,     1.0);
    vec4 clipPrev = u_PrevViewProjection * vec4(v_PrevWorldPos, 1.0);
    vec2 ndcCurr = clipCurr.xy / clipCurr.w;
    vec2 ndcPrev = clipPrev.xy / clipPrev.w;
    o_Velocity = (ndcCurr - ndcPrev) * 0.5;
}

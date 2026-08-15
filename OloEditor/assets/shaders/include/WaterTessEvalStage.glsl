// WaterTessEvalStage.glsl — shared stage body for Water.glsl and Water_Depth.glsl.
// The surface-depth capture replays the SAME displacement chain as the
// color pass (barycentric interp + Gerstner/FFT displacement); any drift between the two
// puts the underwater fog's captured surface at a different height than
// the drawn one. Included by both after their own `#type`/`#version`
// lines; sibling includes resolve inside include/.

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
    // Reconstruction flavour of u_Projection (#691 Phase 8) — every stage's
    // declaration must match or glLinkProgram rejects the program; only the
    // fragment stage reads it. Identical to u_Projection on GL.
    mat4 u_ProjectionForReconstruction;
};

#include "InstanceBlock_Single.glsl"

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

#include "WaterCommon.glsl"

// FFT ocean cascade textures (WATER_FUTURE_IMPROVEMENTS.md §1).
#include "BindlessHeap.glsl"
#ifdef OLO_BINDLESS
#define u_FFTDisplacement OLO_HEAP_TEX_2D(50)  // rgb = (dx, h, dz), a = foam — TEX_WATER_FFT_DISPLACEMENT
#define u_FFTDerivatives OLO_HEAP_TEX_2D(51)  // rgb = normal, a = jacobian — TEX_WATER_FFT_DERIVATIVES
#else
layout(binding = 50) uniform sampler2D u_FFTDisplacement; // rgb = (dx, h, dz), a = foam
layout(binding = 51) uniform sampler2D u_FFTDerivatives;  // rgb = normal, a = jacobian
#endif

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
        // Zero/NaN-safe — see the vertex-stage FFT branch.
        vec3 fftNormal = textureLod(u_FFTDerivatives, fftUV, 0.0).xyz;
        displacedNormal = (dot(fftNormal, fftNormal) > 1e-12) ? normalize(fftNormal) : vec3(0.0, 1.0, 0.0);
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

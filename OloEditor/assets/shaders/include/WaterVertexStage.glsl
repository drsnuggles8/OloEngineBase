// WaterVertexStage.glsl — shared stage body for Water.glsl and Water_Depth.glsl.
// The surface-depth capture replays the SAME displacement chain as the
// color pass (Gerstner/FFT displacement, camera-relative origin); any drift between the two
// puts the underwater fog's captured surface at a different height than
// the drawn one. Included by both after their own `#type`/`#version`
// lines; sibling includes resolve inside include/.

#ifdef OLO_VULKAN
// #691 (ADR 0011 §5): V1 engine-vertex pull. Binding 57 is the
// engine-wide vertex-pull binding and the root struct carries this buffer's
// device address; the stream is the engine `Vertex` (32 B: vec3 position @0,
// vec3 normal @12, vec2 uv @24), so the per-vertex stride is 8 floats. This is
// the VERTEX stage only -- the tessellation stages consume this stage's
// varyings, not vertex attributes, so they need no branch (A10).
layout(std430, binding = 57) readonly buffer OloVertexPull
{
    float v[];
} b_Vertices;
#define OLO_PULLED_VERTEX 1
#else
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord;
#endif

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
    // Reconstruction flavour of u_Projection (#691) — every stage's
    // declaration must match or glLinkProgram rejects the program; only the
    // fragment stage reads it. Identical to u_Projection on GL.
    mat4 u_ProjectionForReconstruction;
};

// Model UBO (binding 3) — the Single variant, like this shader's TES/FS:
// water is single-instance by design, and the varying-producing include
// would declare a v_InstanceIndex output no later stage consumes (a
// per-pipeline Vulkan validation interface warning).
#include "InstanceBlock_Single.glsl"

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

#include "WaterCommon.glsl"

// FFT ocean cascade textures (WATER_FUTURE_IMPROVEMENTS.md §1). Sampled when
// u_FFTParams.x > 0.5 instead of summing Gerstner waves analytically.
#include "BindlessHeap.glsl"
#ifdef OLO_BINDLESS
#define u_FFTDisplacement OLO_HEAP_TEX_2D(50)  // rgb = (dx, h, dz), a = foam — TEX_WATER_FFT_DISPLACEMENT
#define u_FFTDerivatives OLO_HEAP_TEX_2D(51)  // rgb = normal, a = jacobian — TEX_WATER_FFT_DERIVATIVES
#else
layout(binding = 50) uniform sampler2D u_FFTDisplacement; // rgb = (dx, h, dz), a = foam
layout(binding = 51) uniform sampler2D u_FFTDerivatives;  // rgb = normal, a = jacobian
#endif

layout(location = 0) out vec3 v_WorldPos;
layout(location = 1) out vec3 v_Normal;
layout(location = 2) out vec2 v_TexCoord;
layout(location = 3) out vec3 v_ViewDir;
layout(location = 4) out vec3 v_Tangent;
layout(location = 5) out vec3 v_Bitangent;
#ifndef OLO_VULKAN
// Non-tess fallback only (VS feeds FS directly). Under Vulkan water always
// tessellates and the TES computes its own v_WaveHeight — declaring it here
// would be a written-but-unconsumed VS->TCS interface warning per pipeline.
layout(location = 6) out float v_WaveHeight;
#endif
// Previous-frame world position (wave + model reprojection) for RT3 velocity.
layout(location = 7) out vec3 v_PrevWorldPos;

void main()
{
#ifdef OLO_PULLED_VERTEX
    int vertBase = gl_VertexIndex * 8;
    vec3 a_Position = vec3(b_Vertices.v[vertBase + 0], b_Vertices.v[vertBase + 1], b_Vertices.v[vertBase + 2]);
    vec3 a_Normal = vec3(b_Vertices.v[vertBase + 3], b_Vertices.v[vertBase + 4], b_Vertices.v[vertBase + 5]);
    vec2 a_TexCoord = vec2(b_Vertices.v[vertBase + 6], b_Vertices.v[vertBase + 7]);
#endif
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
#ifndef OLO_VULKAN
        v_WaveHeight = 0.0;
#endif
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
        // Zero/NaN-safe: a zero or non-finite derivatives texel would make
        // normalize() emit NaN into the whole triangle's interpolants.
        vec3 fftNormal = textureLod(u_FFTDerivatives, fftUV, 0.0).xyz;
        displacedNormal = (dot(fftNormal, fftNormal) > 1e-12) ? normalize(fftNormal) : vec3(0.0, 1.0, 0.0);
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
#ifndef OLO_VULKAN
    v_WaveHeight = (u_FFTParams.x > 0.5) ? (displacedPos.y - worldPos.y)
                                         : (displacedPos.y - worldPos.y) / maxAmplitude;
#endif

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

// =============================================================================
// AtmosphereSky.glsl - Combined day/night atmosphere sky (issue #633)
//
// One bake pass renders the Preetham day sky, a procedural starfield, the
// moon disk + glow, the twilight cross-fade between them, and an optional
// bake-time cloud tint (so IBL / SSR / water reflections see the cloudscape)
// into one face of a cubemap. Driven by AtmosphereSkyUBO (AtmosphereSky.h),
// whose day half is byte-identical to PreethamCoefficientsUBO — the Perez
// math below matches ProceduralSky.glsl / ProceduralSky.cpp exactly, and the
// night-layer helpers are mirrored CPU-side in AtmosphereSky.cpp
// (NightLayer / StarField / CloudFBM) for headless tests.
// =============================================================================

#type vertex
#version 460 core

#ifdef OLO_VULKAN
// #691 (ADR 0011 §5): V1 engine-vertex pull. On the Vulkan route the
// pipeline has no vertex-input state, so attributes are READ from binding 57
// (the engine-wide vertex-pull binding; the root struct carries this buffer's
// device address). This bake draws MeshPrimitives::CreateSkyboxCube(), whose
// stream is the engine `Vertex` (32 B: vec3 position @0, vec3 normal @12,
// vec2 uv @24), so the stride is 8 floats even though this stage only needs
// the position. The GL attribute branch below is untouched.
layout(std430, binding = 57) readonly buffer OloVertexPull
{
    float v[];
} b_Vertices;
#define OLO_PULLED_VERTEX 1
#else
layout(location = 0) in vec3 a_Position;
#endif

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
#ifdef OLO_PULLED_VERTEX
    int vertBase = gl_VertexIndex * 8;
    vec3 a_Position = vec3(b_Vertices.v[vertBase + 0], b_Vertices.v[vertBase + 1], b_Vertices.v[vertBase + 2]);
#endif
    v_LocalPos = a_Position;
    gl_Position = u_ViewProjection * vec4(a_Position, 1.0);
}

#type fragment
#version 460 core

layout(location = 0) in vec3 v_LocalPos;
layout(location = 0) out vec4 o_Color;

// Must match ShaderBindingLayout::UBO_ATMOSPHERE_SKY (52).
layout(std140, binding = 52) uniform AtmosphereSkyData {
    // Day half — identical layout to ProceduralSkyData (PreethamCoefficientsUBO).
    vec4 u_SunDirection;  // xyz = toward-sun unit vec, w = cos(angular radius * disk size)
    vec4 u_ZenithXYY;     // x = chromaticity x, y = chromaticity y, z = luminance Y, w = unused
    vec4 u_A;
    vec4 u_B;
    vec4 u_C;
    vec4 u_D;
    vec4 u_E;
    vec4 u_Params;        // x = exposure, y = sun intensity, z = show sun disk (0/1), w = unused
    // Night half.
    vec4 u_MoonDirection; // xyz = toward moon (unit), w = cos(angular radius * disk size)
    vec4 u_NightParams;   // x = night blend, y = star intensity, z = moon intensity, w = night brightness
    vec4 u_NightParams2;  // x = star rotation (rad), y = moon illuminated fraction,
                          // z = cloud coverage, w = cloud wetness
};

// ── Day half: Perez / xyY — identical to ProceduralSky.glsl ──

float PerezF(float A, float B, float C, float D, float E,
             float cosTheta, float cosGamma, float gamma)
{
    float safeCos = max(cosTheta, 1e-3);
    float t1 = 1.0 + A * exp(B / safeCos);
    float t2 = 1.0 + C * exp(D * gamma) + E * cosGamma * cosGamma;
    return t1 * t2;
}

vec3 XYYToLinearRGB(float cx, float cy, float Y)
{
    float safeY = max(cy, 1e-6);
    float X = (cx / safeY) * Y;
    float Z = ((1.0 - cx - cy) / safeY) * Y;
    return vec3(
         3.2404542 * X - 1.5371385 * Y - 0.4985314 * Z,
        -0.9692660 * X + 1.8760108 * Y + 0.0415560 * Z,
         0.0556434 * X - 0.2040259 * Y + 1.0572252 * Z
    );
}

vec3 dayLayer(vec3 viewDir)
{
    vec3 sunDir = normalize(u_SunDirection.xyz);

    float cosTheta = max(viewDir.y, 0.0);
    float cosGamma = clamp(dot(viewDir, sunDir), -1.0, 1.0);
    float gamma = acos(cosGamma);

    float cosThetaS = clamp(sunDir.y, 0.0, 1.0);
    float thetaS = acos(cosThetaS);

    float FzX = PerezF(u_A.x, u_B.x, u_C.x, u_D.x, u_E.x, 1.0, cosThetaS, thetaS);
    float FzY = PerezF(u_A.y, u_B.y, u_C.y, u_D.y, u_E.y, 1.0, cosThetaS, thetaS);
    float FzL = PerezF(u_A.z, u_B.z, u_C.z, u_D.z, u_E.z, 1.0, cosThetaS, thetaS);

    float FX = PerezF(u_A.x, u_B.x, u_C.x, u_D.x, u_E.x, cosTheta, cosGamma, gamma);
    float FY = PerezF(u_A.y, u_B.y, u_C.y, u_D.y, u_E.y, cosTheta, cosGamma, gamma);
    float FL = PerezF(u_A.z, u_B.z, u_C.z, u_D.z, u_E.z, cosTheta, cosGamma, gamma);

    float cx = u_ZenithXYY.x * (FX / max(FzX, 1e-6));
    float cy = u_ZenithXYY.y * (FY / max(FzY, 1e-6));
    float Y  = u_ZenithXYY.z * (FL / max(FzL, 1e-6));

    float Yt = 1.0 - exp(-u_Params.x * Y);
    vec3 skyRGB = XYYToLinearRGB(cx, cy, Yt);

    if (u_Params.z > 0.5 && cosGamma > u_SunDirection.w)
    {
        float t = clamp((cosGamma - u_SunDirection.w) /
                        max(1.0 - u_SunDirection.w, 1e-6), 0.0, 1.0);
        skyRGB += vec3(1.0, 0.95, 0.9) * (8.0 * u_Params.y * smoothstep(0.0, 1.0, t));
    }

    return max(skyRGB, vec3(0.0));
}

// ── Night half — mirrored CPU-side in AtmosphereSky.cpp ──
//
// GOLDEN COUPLING: the star hash / positions / brightness below feed the
// Atmosphere_Night{Clear,Overcast,Storm} visual goldens
// (OloEngine/tests/Rendering/PropertyTests/AtmosphereVisualEvidenceTest.cpp).
// Any change here relocates or re-lights the star field, so it MUST rebake
// those goldens in the SAME PR — run the test with --olo-golden-rebase
// on the baseline GPU. Skipping it leaves the night cells permanently red
// (see issue #754: f4fef24b changed this hash but did not rebase, and the
// red normalised as "the expected failure" for 10 days).

// Integer bit-mixer (PCG output permutation). Mirrors PcgHash
// (AtmosphereSky.cpp) EXACTLY: unsigned wrap, shift and xor are bit-defined
// operations, so every vendor and the CPU produce identical results.
//
// This replaces the classic `fract(sin(dot(p, k)) * 43758.5453)` hash, which
// is NOT portable. That hash feeds sin() an argument in the tens of thousands,
// where a 1-ULP difference in the argument moves the result by a large
// fraction of a period; NVIDIA and Mesa do not implement sin() to identical
// precision there, so the *= 43758 and fract() amplified the disagreement into
// completely different values. The stars therefore landed in different places
// on different GPUs, and the C++ mirror (std::sin) could match neither.
uint pcgHash(uint v)
{
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// Mirrors HashCell (AtmosphereSky.cpp). Integer lattice cell -> uint.
uint hashCell(ivec3 c, uint seed)
{
    uint h = pcgHash(uint(c.x) ^ 0x9E3779B9u);
    h = pcgHash(h ^ uint(c.y) ^ 0x85EBCA6Bu);
    h = pcgHash(h ^ uint(c.z) ^ 0xC2B2AE35u);
    return pcgHash(h ^ seed);
}

// Mirrors Hash1 (AtmosphereSky.cpp). Result in [0,1).
// Masked to 24 bits so the uint->float conversion is EXACT on every
// implementation (a float mantissa holds 24 bits), and scaled by a power of
// two so the divide introduces no rounding of its own.
float hash1(ivec3 c, uint seed)
{
    return float(hashCell(c, seed) & 0xFFFFFFu) * (1.0 / 16777216.0);
}

// Mirrors Hash3 (AtmosphereSky.cpp).
vec3 hash3(ivec3 c)
{
    return vec3(hash1(c, 0u), hash1(c, 1u), hash1(c, 2u));
}

// Mirrors StarField (AtmosphereSky.cpp).
float starField(vec3 dir, float rotation, float intensity)
{
    float c = cos(rotation);
    float s = sin(rotation);
    vec3 d = vec3(c * dir.x + s * dir.z, dir.y, -s * dir.x + c * dir.z);

    // `dir` is unit, so p stays within +/-60 and the cell index converts to
    // int exactly. A 1-ULP disagreement in cos/sin between vendors now only
    // matters exactly on a cell boundary, instead of re-rolling every star.
    vec3 p = d * 60.0;
    vec3 cellF = floor(p);
    ivec3 cell = ivec3(cellF);
    vec3 f = p - cellF;
    vec3 starPos = hash3(cell);
    float dist = length(f - starPos);
    float lum = pow(hash1(cell, 3u), 14.0);
    float star = (1.0 - smoothstep(0.0, 0.18, dist)) * lum;
    return star * intensity * 60.0;
}

// Mirrors NightLayer (AtmosphereSky.cpp).
vec3 nightLayer(vec3 dir)
{
    vec3 moonDir = u_MoonDirection.xyz;
    float starIntensity = u_NightParams.y;
    float moonIntensity = u_NightParams.z;
    float nightBrightness = u_NightParams.w;
    float starRotation = u_NightParams2.x;
    float moonIllum = u_NightParams2.y;

    float up = clamp(dir.y, 0.0, 1.0);
    vec3 night = mix(vec3(0.035, 0.045, 0.085), vec3(0.010, 0.013, 0.026), up);
    night *= 1.0 + 1.5 * moonIllum * min(moonIntensity, 1.0);

    float cosGamma = clamp(dot(dir, moonDir), -1.0, 1.0);
    night += vec3(0.28, 0.33, 0.42) * (pow(max(cosGamma, 0.0), 24.0) * 0.6 * moonIntensity);

    if (cosGamma > u_MoonDirection.w)
    {
        float t = clamp((cosGamma - u_MoonDirection.w) /
                        max(1.0 - u_MoonDirection.w, 1e-6), 0.0, 1.0);
        night += vec3(0.95, 0.93, 0.88) *
                 (4.0 * moonIntensity * (0.2 + 0.8 * moonIllum) * smoothstep(0.0, 1.0, t));
    }

    night += vec3(0.9, 0.95, 1.0) *
             (starField(dir, starRotation, starIntensity) * smoothstep(-0.05, 0.15, dir.y));

    return night * nightBrightness;
}

// ── Bake-time cloud tint — mirrors Hash12/ValueNoise2D/CloudFBM (AtmosphereSky.cpp) ──

float hash12(vec2 p)
{
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float valueNoise2D(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = p - i;
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = hash12(i);
    float b = hash12(i + vec2(1.0, 0.0));
    float c = hash12(i + vec2(0.0, 1.0));
    float d = hash12(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

float cloudFBM(vec2 p)
{
    float sum = 0.0;
    float amp = 0.5;
    vec2 q = p;
    for (int i = 0; i < 3; ++i)
    {
        sum += amp * valueNoise2D(q);
        q *= 2.17;
        amp *= 0.5;
    }
    return sum;
}

void main()
{
    vec3 viewDir = normalize(v_LocalPos);
    float nightBlend = clamp(u_NightParams.x, 0.0, 1.0);

    vec3 day = (nightBlend < 1.0) ? dayLayer(viewDir) : vec3(0.0);
    vec3 night = (nightBlend > 0.0) ? nightLayer(viewDir) : vec3(0.0);
    vec3 sky = mix(day, night, nightBlend);

    float coverage = u_NightParams2.z;
    if (coverage > 0.0 && viewDir.y > 0.02)
    {
        vec2 uv = viewDir.xz / (viewDir.y + 0.18) * 0.55;
        float fbm = cloudFBM(uv);
        // Damped to a TINT (max ~0.7 opacity at full storm coverage): the
        // real clouds are the raymarched layer — this only keeps IBL and
        // reflections from staying cloudless-blue under an overcast sky.
        // Tuned live: an undamped mix washed the whole cubemap white.
        float cloud = smoothstep(1.05 - coverage, 1.25 - coverage, fbm) *
                      smoothstep(0.02, 0.12, viewDir.y) *
                      (0.25 + 0.45 * coverage);
        float wetnessDarken = 1.0 - 0.55 * u_NightParams2.w;
        vec3 dayCloud = vec3(0.82, 0.83, 0.86) * wetnessDarken;
        vec3 nightCloud = vec3(0.05, 0.06, 0.08) * u_NightParams.w;
        sky = mix(sky, mix(dayCloud, nightCloud, nightBlend), cloud);
    }

    o_Color = vec4(max(sky, vec3(0.0)), 1.0);
}

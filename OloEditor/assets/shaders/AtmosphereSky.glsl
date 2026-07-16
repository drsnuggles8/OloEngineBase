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

// Must match ShaderBindingLayout::UBO_ATMOSPHERE_SKY (51).
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

// Mirrors Hash13 (AtmosphereSky.cpp).
float hash13(vec3 p)
{
    return fract(sin(dot(p, vec3(127.1, 311.7, 74.7))) * 43758.5453);
}

// Mirrors Hash33 (AtmosphereSky.cpp).
vec3 hash33(vec3 p)
{
    return vec3(hash13(p), hash13(p + vec3(19.19, 0.0, 0.0)), hash13(p + vec3(0.0, 47.31, 0.0)));
}

// Mirrors StarField (AtmosphereSky.cpp).
float starField(vec3 dir, float rotation, float intensity)
{
    float c = cos(rotation);
    float s = sin(rotation);
    vec3 d = vec3(c * dir.x + s * dir.z, dir.y, -s * dir.x + c * dir.z);

    vec3 p = d * 60.0;
    vec3 cell = floor(p);
    vec3 f = p - cell;
    vec3 starPos = hash33(cell);
    float dist = length(f - starPos);
    float lum = pow(hash13(cell + vec3(17.0)), 14.0);
    float star = smoothstep(0.18, 0.0, dist) * lum;
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

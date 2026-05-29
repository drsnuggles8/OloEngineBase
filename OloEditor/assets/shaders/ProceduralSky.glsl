// =============================================================================
// ProceduralSky.glsl - Preetham 1999 Analytic Daylight Model
// Renders the sky into one face of a cubemap; the cubemap is then consumed
// by the existing skybox + IBL pipeline.
//
// Reference: A. J. Preetham, P. Shirley, B. Smits,
// "A Practical Analytic Model for Daylight", SIGGRAPH 1999.
//
// Driven by PreethamCoefficientsUBO (see ProceduralSky.h). The C++ side
// pre-computes the Perez F-function coefficients (linear in turbidity) and
// the zenith chromaticity xyY (polynomial in turbidity and solar zenith
// angle). The shader applies the Perez distribution per pixel and converts
// the xyY result to linear sRGB.
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

// Must match ShaderBindingLayout::UBO_PROCEDURAL_SKY (36).
layout(std140, binding = 36) uniform ProceduralSkyData {
    vec4 u_SunDirection;  // xyz = toward-sun unit vec, w = cos(angular radius * disk size)
    vec4 u_ZenithXYY;     // x = chromaticity x, y = chromaticity y, z = luminance Y (cd/m^2), w = unused
    vec4 u_A;             // Perez A coefficients packed as (Fx.A, Fy.A, FY.A, _)
    vec4 u_B;
    vec4 u_C;
    vec4 u_D;
    vec4 u_E;
    vec4 u_Params;        // x = exposure, y = sun intensity, z = show sun disk (0/1), w = unused
};

// Perez F distribution. Identical to the C++ reference implementation in
// ProceduralSky.cpp so the test that pins
// EvaluateAtDirection(...) against a known direction also pins this code.
float PerezF(float A, float B, float C, float D, float E,
             float cosTheta, float cosGamma, float gamma)
{
    // Avoid blow-up at the horizon: exp(B/cos(theta)) -> inf as cos -> 0.
    float safeCos = max(cosTheta, 1e-3);
    float t1 = 1.0 + A * exp(B / safeCos);
    float t2 = 1.0 + C * exp(D * gamma) + E * cosGamma * cosGamma;
    return t1 * t2;
}

// CIE xyY -> linear sRGB (D65), standard matrix from the sRGB spec.
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

void main()
{
    // World-space view direction (we render a unit cube centered at the origin
    // with a 90-degree FOV projection — vertex position is the sample direction).
    vec3 viewDir = normalize(v_LocalPos);

    vec3 sunDir = normalize(u_SunDirection.xyz);

    // Clamp below-horizon view directions to a tiny positive cos(theta) so
    // the Perez F-function stays bounded. Sub-horizon pixels (ground) get
    // the horizon value, which is the cheapest valid placeholder before
    // proper ground tinting lands.
    float cosTheta = max(viewDir.y, 0.0);
    float cosGamma = clamp(dot(viewDir, sunDir), -1.0, 1.0);
    float gamma = acos(cosGamma);

    // Reference samples at the zenith (theta = 0) and the sun direction (gamma = thetaS).
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

    // Tonemap the LUMINANCE (not the RGB) so the sky's huge zenith->horizon
    // dynamic range is compressed into [0,1) while the chromaticity (cx, cy)
    // is left untouched. Feeding the raw absolute luminance to the scene
    // tonemapper instead clips the bright horizon/sun regions to white and
    // desaturates the blue. `1 - exp(-exposure*Y)` is the standard Preetham
    // display transform (Preetham 1999, Hoffman/Preetham course notes).
    float Yt = 1.0 - exp(-u_Params.x * Y);
    vec3 skyRGB = XYYToLinearRGB(cx, cy, Yt);

    // Optional bright sun disk, added in HDR on top of the [0,1] sky so it
    // can drive bloom. SunIntensity scales it.
    if (u_Params.z > 0.5 && cosGamma > u_SunDirection.w)
    {
        // Soft falloff inside the disk to mask aliasing on the sun edge.
        float t = clamp((cosGamma - u_SunDirection.w) /
                        max(1.0 - u_SunDirection.w, 1e-6), 0.0, 1.0);
        skyRGB += vec3(1.0, 0.95, 0.9) * (8.0 * u_Params.y * smoothstep(0.0, 1.0, t));
    }

    skyRGB = max(skyRGB, vec3(0.0));
    o_Color = vec4(skyRGB, 1.0);
}

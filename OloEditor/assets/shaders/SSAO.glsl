#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;

layout(location = 0) out vec2 v_TexCoord;

void main()
{
    v_TexCoord = a_TexCoord;
    gl_Position = vec4(a_Position, 1.0);
}

#type fragment
#version 460 core

// SSAO — Screen-Space Ambient Occlusion (normal-oriented hemisphere obscurance).
//
// For each opaque pixel we walk a noise-rotated spiral of taps inside the
// screen-space projection of the world-space radius, reconstruct each tap's
// view-space position from depth, and measure how far the tap sits ABOVE this
// pixel's tangent plane (the cosine dot(normal, dir) past a small bias). A flat
// surface keeps every neighbouring tap in its own tangent plane (dot ~= 0), so
// it does NOT self-occlude — the earlier horizon variant measured horizons in
// camera-elevation space without subtracting the surface tangent and therefore
// darkened flat ground at any grazing/tilted angle. A crease, wall or contact
// lifts taps above the plane -> occlusion. A WORLD-SPACE range falloff discards
// taps beyond the radius (so the foreshortened far end of a grazing disk can't
// add phantom occlusion). Output R channel = AO (1 = unoccluded, 0 = fully
// occluded); intensity is applied later in PostProcess_SSAOApply.

layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec2 v_TexCoord;

// Scene depth (from ScenePass)
layout(binding = 19) uniform sampler2D u_DepthTexture;

// View-space normals (from ScenePass G-buffer, octahedral encoded in RG16F, attachment 2)
layout(binding = 22) uniform sampler2D u_NormalsTexture;

// 4x4 random rotation noise texture (unit vectors)
layout(binding = 21) uniform sampler2D u_NoiseTexture;

// SSAO UBO (binding 9)
layout(std140, binding = 9) uniform SSAOUBO
{
    float u_Radius;
    float u_Bias;
    float u_Intensity;
    int   u_Samples;

    int   u_ScreenWidth;
    int   u_ScreenHeight;
    int   u_DebugView;
    float _pad1;

    mat4  u_Projection;
    mat4  u_InverseProjection;
};

const float PI = 3.14159265359;
const float TWO_PI = 6.28318530718;

// Octahedral decode: RG16F [-1,1]² → unit normal on sphere
vec3 octDecode(vec2 f)
{
    vec3 n = vec3(f.xy, 1.0 - abs(f.x) - abs(f.y));
    float t = max(-n.z, 0.0);
    n.x += (n.x >= 0.0) ? -t : t;
    n.y += (n.y >= 0.0) ? -t : t;
    return normalize(n);
}

// Reconstruct view-space position from depth and screen UV
vec3 reconstructViewPos(vec2 uv, float depth)
{
    vec4 clipPos = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 viewPos = u_InverseProjection * clipPos;
    return viewPos.xyz / viewPos.w;
}

// Interleaved gradient noise for per-pixel radial jitter (Jorge Jimenez, 2014)
float interleavedGradientNoise(vec2 pixelCoord)
{
    vec3 magic = vec3(0.06711056, 0.00583715, 52.9829189);
    return fract(magic.z * fract(dot(pixelCoord, magic.xy)));
}

// World-space AO radius projected to a pixel count at the given view depth.
float projectedRadiusInPixels(float viewZ)
{
    // u_Projection[1][1] is the Y scale factor (1 / tan(fov/2))
    return (u_Radius * u_Projection[1][1] * float(u_ScreenHeight)) / (2.0 * abs(viewZ));
}

void main()
{
    float depth = texture(u_DepthTexture, v_TexCoord).r;

    // Skip skybox / far plane — no surface to occlude.
    if (depth >= 1.0)
    {
        o_Color = vec4(1.0, 0.0, 0.0, 1.0);
        return;
    }

    // Read octahedral-encoded normal — sentinel value (-2,-2) means "no normal".
    vec2 encodedNormal = texture(u_NormalsTexture, v_TexCoord).rg;
    if (encodedNormal.x < -1.5)
    {
        o_Color = vec4(1.0, 0.0, 0.0, 1.0);
        return;
    }

    vec3 viewPos = reconstructViewPos(v_TexCoord, depth);
    vec3 viewNormal = octDecode(encodedNormal);

    // Projected radius in pixels — clamp to avoid oversampling. Below 1px the
    // world radius is sub-texel (distant surface) so AO contributes nothing.
    float radiusPixels = projectedRadiusInPixels(viewPos.z);
    if (radiusPixels < 1.0)
    {
        o_Color = vec4(1.0, 0.0, 0.0, 1.0);
        return;
    }
    radiusPixels = min(radiusPixels, 256.0);

    // Per-4x4-block angular rotation + per-pixel radial jitter to break up the
    // fixed spiral pattern (the bilateral blur cleans up the residual noise).
    vec2 pixelCoord = v_TexCoord * vec2(float(u_ScreenWidth), float(u_ScreenHeight));
    vec2 noiseVec = texture(u_NoiseTexture, pixelCoord / 4.0).rg;
    float rotAngle = atan(noiseVec.y, noiseVec.x);
    float radialJitter = interleavedGradientNoise(pixelCoord);

    int numSamples = clamp(u_Samples, 8, 64);
    vec2 texelSize = 1.0 / vec2(float(u_ScreenWidth), float(u_ScreenHeight));

    const float kNumTurns = 7.0;     // spiral turns across the disk
    const float kMinCos = 0.1;       // ignore taps within ~this cosine of the tangent plane (rejects grazing same-surface taps that would otherwise self-occlude flat ground)
    const float kBiasWindow = 0.25;  // cosine width above the threshold over which a tap ramps to full weight
    const float kNearScale = 0.3;    // proximity emphasis: taps beyond ~sqrt(kNearScale)*radius are down-weighted, so a NEAR crease/wall dominates over far grazing taps
    const float kStrength = 4.0;     // internal obscurance gain (AO punch); final strength still scaled by u_Intensity downstream
    float biasFloor = kMinCos + u_Bias;
    float radius2 = u_Radius * u_Radius;

    float occlusion = 0.0;

    for (int i = 0; i < numSamples; ++i)
    {
        // Spiral tap: angle winds kNumTurns times, radius grows ~uniformly over
        // the disk (sqrt keeps area density even). Rotated/jittered per pixel.
        float t = (float(i) + 0.5) / float(numSamples);
        float angle = t * kNumTurns * TWO_PI + rotAngle;
        float r = sqrt(min(t + (radialJitter - 0.5) / float(numSamples), 1.0));
        vec2 sampleUV = v_TexCoord + vec2(cos(angle), sin(angle)) * (r * radiusPixels) * texelSize;
        if (sampleUV.x < 0.0 || sampleUV.x > 1.0 || sampleUV.y < 0.0 || sampleUV.y > 1.0)
            continue;

        float sampleDepth = texture(u_DepthTexture, sampleUV).r;
        if (sampleDepth >= 1.0)
            continue; // sky tap — unoccluded

        vec3 samplePos = reconstructViewPos(sampleUV, sampleDepth);
        vec3 toSample = samplePos - viewPos;
        float dist2 = dot(toSample, toSample);
        if (dist2 < 1e-8)
            continue;

        float invLen = inversesqrt(dist2);
        // How far the tap sits above this pixel's tangent plane (cosine).
        float ndotv = dot(viewNormal, toSample) * invLen;
        // Fade taps as they approach / pass the world-space radius so a
        // foreshortened grazing disk cannot reach distant geometry, AND weight
        // closer taps more so a near crease/wall outweighs the far grazing floor.
        float rangeFalloff = 1.0 - smoothstep(radius2 * 0.64, radius2, dist2);
        float proximity = 1.0 / (1.0 + dist2 / (kNearScale * radius2));
        occlusion += smoothstep(biasFloor, biasFloor + kBiasWindow, ndotv) * rangeFalloff * proximity;
    }

    float ao = 1.0 - clamp(kStrength * occlusion / float(numSamples), 0.0, 1.0);

    o_Color = vec4(ao, 0.0, 0.0, 1.0);
}

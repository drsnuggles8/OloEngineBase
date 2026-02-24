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

// GTAO — Ground Truth Ambient Occlusion
// Horizon-based screen-space AO using G-buffer normals (octahedral encoded in RG16F).
// Based on Jimenez et al. "Practical Real-Time Strategies for Accurate Indirect Occlusion" (2016).

layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec2 v_TexCoord;

// Scene depth (from ScenePass)
layout(binding = 19) uniform sampler2D u_DepthTexture;

// View-space normals (from ScenePass G-buffer, octahedral encoded in RG16F, attachment 2)
layout(binding = 22) uniform sampler2D u_NormalsTexture;

// 4x4 random rotation noise texture
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
const float HALF_PI = 1.57079632679;

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

// Interleaved gradient noise for per-pixel jitter (Jorge Jimenez, 2014)
float interleavedGradientNoise(vec2 pixelCoord)
{
    vec3 magic = vec3(0.06711056, 0.00583715, 52.9829189);
    return fract(magic.z * fract(dot(pixelCoord, magic.xy)));
}

// Compute the inverse of the projected radius in screen pixels at given view depth
float projectedRadiusInPixels(float viewZ)
{
    // u_Projection[1][1] is the Y scale factor (1 / tan(fov/2))
    return (u_Radius * u_Projection[1][1] * float(u_ScreenHeight)) / (2.0 * abs(viewZ));
}

// Fast atan approximation for horizon angle computation
float fastAcos(float x)
{
    // Polynomial approximation (max error ~0.02 rad)
    float ax = abs(x);
    float res = -0.156583 * ax + HALF_PI;
    res *= sqrt(1.0 - ax);
    return (x >= 0.0) ? res : PI - res;
}

// Integrate the cosine-weighted visible angle for a single horizon slice
// n_dot_v: dot(normal, viewDir), sinN/cosN: sin/cos of normal projected angle in slice
float integrateArc(float h1, float h2, float sinN, float cosN)
{
    // Integrated form of cos(theta - n) over [h1, h2]
    float a1 = -cos(2.0 * h1 - acos(cosN)) + cosN + 2.0 * h1 * sinN;
    float a2 = -cos(2.0 * h2 - acos(cosN)) + cosN + 2.0 * h2 * sinN;
    return 0.25 * (a1 + a2);
}

void main()
{
    float depth = texture(u_DepthTexture, v_TexCoord).r;

    // Skip skybox / far plane
    if (depth >= 1.0)
    {
        o_Color = vec4(1.0, 0.0, 0.0, 1.0);
        return;
    }

    // Read octahedral-encoded normal — sentinel value (-2,-2) means "no normal"
    vec2 encodedNormal = texture(u_NormalsTexture, v_TexCoord).rg;
    if (encodedNormal.x < -1.5)
    {
        o_Color = vec4(1.0, 0.0, 0.0, 1.0);
        return;
    }

    // Reconstruct view-space position and decode normal
    vec3 viewPos = reconstructViewPos(v_TexCoord, depth);
    vec3 viewNormal = octDecode(encodedNormal);
    vec3 viewDir = normalize(-viewPos);

    // Compute projected radius in pixels — clamp to avoid oversampling
    float radiusPixels = projectedRadiusInPixels(viewPos.z);
    if (radiusPixels < 1.0)
    {
        o_Color = vec4(1.0, 0.0, 0.0, 1.0);
        return;
    }
    radiusPixels = min(radiusPixels, 256.0);

    // Per-pixel noise rotation and spatial jitter
    vec2 pixelCoord = v_TexCoord * vec2(float(u_ScreenWidth), float(u_ScreenHeight));
    vec2 noiseVec = texture(u_NoiseTexture, pixelCoord / 4.0).rg;
    float noiseAngle = noiseVec.x * PI;
    float noiseOffset = noiseVec.y;

    // Number of direction slices and steps per slice
    int numSlices = clamp(u_Samples / 4, 2, 16);
    int stepsPerSlice = clamp(u_Samples / numSlices, 2, 16);

    vec2 texelSize = 1.0 / vec2(float(u_ScreenWidth), float(u_ScreenHeight));

    float occlusion = 0.0;

    for (int slice = 0; slice < numSlices; ++slice)
    {
        // Direction angle for this slice, rotated by per-pixel noise
        float phi = (PI / float(numSlices)) * (float(slice) + noiseOffset) + noiseAngle;
        vec2 dir = vec2(cos(phi), sin(phi));
        vec2 stepDir = dir * texelSize;

        // Project the normal onto the slice plane to get the normal's angle in this slice
        // sliceDir in view space (approximate: we use screen-space direction mapped to view)
        vec3 sliceDirVS = vec3(dir.x, dir.y, 0.0);
        vec3 orthoDir = normalize(cross(sliceDirVS, vec3(0.0, 0.0, 1.0)));
        vec3 projNormal = viewNormal - orthoDir * dot(viewNormal, orthoDir);
        float projNormalLen = length(projNormal);

        // The angle of the projected normal relative to the view direction in the slice plane
        float cosNormalAngle = clamp(dot(normalize(projNormal), viewDir), -1.0, 1.0);
        float normalAngle = -sign(dot(projNormal, sliceDirVS)) * fastAcos(cosNormalAngle);
        float sinN = sin(normalAngle);
        float cosN = cos(normalAngle);

        // March in both directions along the slice to find max horizon angles
        float h1 = -HALF_PI; // Horizon angle in negative direction
        float h2 = -HALF_PI; // Horizon angle in positive direction

        for (int step = 1; step <= stepsPerSlice; ++step)
        {
            float stepScale = (float(step) + noiseOffset * 0.5) / float(stepsPerSlice);
            float marchPixels = stepScale * radiusPixels;

            // Positive direction
            {
                vec2 sampleUV = v_TexCoord + stepDir * marchPixels;
                if (sampleUV.x >= 0.0 && sampleUV.x <= 1.0 && sampleUV.y >= 0.0 && sampleUV.y <= 1.0)
                {
                    float sampleDepth = texture(u_DepthTexture, sampleUV).r;
                    if (sampleDepth < 1.0)
                    {
                        vec3 samplePos = reconstructViewPos(sampleUV, sampleDepth);
                        vec3 horizonVec = samplePos - viewPos;
                        float horizonLen = length(horizonVec);
                        if (horizonLen > 0.001)
                        {
                            float angle = atan(horizonVec.z / max(length(horizonVec.xy), 0.0001));
                            // Apply thickness bias to prevent self-occlusion
                            angle -= u_Bias;
                            // Range attenuation: reduce contribution for distant samples
                            float rangeAtten = 1.0 - smoothstep(u_Radius * 0.8, u_Radius, horizonLen);
                            // Keep maximum horizon angle (weighted by attenuation)
                            h2 = max(h2, mix(h2, angle, rangeAtten));
                        }
                    }
                }
            }

            // Negative direction
            {
                vec2 sampleUV = v_TexCoord - stepDir * marchPixels;
                if (sampleUV.x >= 0.0 && sampleUV.x <= 1.0 && sampleUV.y >= 0.0 && sampleUV.y <= 1.0)
                {
                    float sampleDepth = texture(u_DepthTexture, sampleUV).r;
                    if (sampleDepth < 1.0)
                    {
                        vec3 samplePos = reconstructViewPos(sampleUV, sampleDepth);
                        vec3 horizonVec = samplePos - viewPos;
                        float horizonLen = length(horizonVec);
                        if (horizonLen > 0.001)
                        {
                            float angle = atan(horizonVec.z / max(length(horizonVec.xy), 0.0001));
                            angle -= u_Bias;
                            float rangeAtten = 1.0 - smoothstep(u_Radius * 0.8, u_Radius, horizonLen);
                            h1 = max(h1, mix(h1, angle, rangeAtten));
                        }
                    }
                }
            }
        }

        // Clamp horizon angles to hemisphere
        h1 = clamp(h1, -HALF_PI, HALF_PI);
        h2 = clamp(h2, -HALF_PI, HALF_PI);

        // Integrate visibility over the arc defined by the two horizon angles
        float sliceAO = integrateArc(h1, h2, sinN, cosN);
        occlusion += sliceAO * projNormalLen;
    }

    // Normalize and apply intensity
    occlusion /= float(numSlices);
    float ao = clamp(occlusion, 0.0, 1.0);

    o_Color = vec4(ao, 0.0, 0.0, 1.0);
}

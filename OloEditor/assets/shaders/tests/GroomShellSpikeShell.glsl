#type vertex
#version 460 core
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_UV;
layout(binding = 0) uniform sampler2D u_LengthDirection;
layout(std140, binding = 30) uniform SpikeParameters
{
    mat4 u_VP;
    mat4 u_InverseVP;
    vec4 u_ViewportFrame;
    vec4 u_Layers;
    vec4 u_Chart;
};
layout(location = 0) flat out float v_Height;
layout(location = 1) out vec2 v_EnvelopeUV;
void main()
{
    float height = float(gl_InstanceIndex + 1) / u_Layers.x;
    // Conservative envelope; fragments intersect the actual jittered shell.
    gl_Position = u_VP * vec4(a_Position + a_Normal * height * u_Chart.w, 1.0);
    v_Height = height;
    v_EnvelopeUV = a_UV;
}
#type fragment
#version 460 core
#include "include/GroomStrandCommon.glsl"
#include "tests/include/GroomShellSpikeIntersection.glsl"
layout(binding = 0) uniform sampler2D u_LengthDirection;
layout(binding = 1) uniform sampler3D u_Density;
layout(std140, binding = 30) uniform SpikeParameters
{
    mat4 u_VP;
    mat4 u_InverseVP;
    vec4 u_ViewportFrame;
    vec4 u_Layers;
    vec4 u_Chart;
};
layout(location = 0) flat in float v_Height;
layout(location = 1) in vec2 v_EnvelopeUV;
layout(location = 0) out vec4 o_Metrics;
void main()
{
    // Derivatives must be evaluated before any divergent silhouette discard.
    vec2 footprint = abs(dFdx(v_EnvelopeUV)) + abs(dFdy(v_EnvelopeUV));
    float lod = max(0.0, log2(max(footprint.x * 64.0, footprint.y * 32.0)));
    uint layer = uint(round(v_Height * u_Layers.x));
    float randomHeight = oloGroomStochasticHash(uint(gl_FragCoord.x), uint(gl_FragCoord.y), uint(u_ViewportFrame.z), layer, 1611u);
    float height = v_Height - (u_ViewportFrame.w > 0.5 ? randomHeight : 0.5) / u_Layers.x;
    vec2 ndc = gl_FragCoord.xy / u_ViewportFrame.xy * 2.0 - 1.0;
    vec4 nearPoint = u_InverseVP * vec4(ndc, -1.0, 1.0);
    vec4 farPoint = u_InverseVP * vec4(ndc, 1.0, 1.0);
    vec3 origin = nearPoint.xyz / nearPoint.w;
    vec3 ray = normalize(farPoint.xyz / farPoint.w - origin);
    float reach = u_Chart.w;
    vec3 position = vec3(0.0);
    vec2 uv = vec2(0.0);
    vec4 map = vec4(0.0);
    for (int iteration = 0; iteration < 4; ++iteration)
    {
        float radius = u_Chart.x + height * reach;
        if (!oloGroomShellSpikeIntersect(origin, ray, radius, gl_FrontFacing, position)) discard;
        uv = vec2(atan(position.y, position.x) / 6.283185307, (position.z - u_Chart.y) / (u_Chart.z - u_Chart.y));
        map = textureLod(u_LengthDirection, uv, 0.0);
        reach = map.x;
    }
    if (uv.y < 0.0 || uv.y > 1.0) discard;
    vec2 sourceUV = uv - height * map.x * map.yz / u_Layers.yz;
    if (sourceUV.y < 0.0 || sourceUV.y > 1.0) discard;
    vec2 occupancy = textureLod(u_Density, vec3(sourceUV, height), lod).rg;
    vec3 normal = normalize(vec3(position.xy, 0.0));
    vec3 tangent = normalize(normal + vec3(-normal.y, normal.x, 0.0) * map.y + vec3(0.0, 0.0, map.z));
    // Swept bands occupy different positions along a fibre and integrate
    // independently under minification. Cross-sections repeated at several
    // heights are correlated. Combining these with a single threshold would
    // either multiply end-on area or divide a tilted fibre's band area by N.
    // Projection of the slab's integrated width*arc-length measure. Multiplying
    // by projected shell area cancels dot(normal,ray), leaving the strand band
    // area width*arc-length*|cross(tangent,ray)|. Offset cylinders have larger
    // texel area than the root chart; compensate by root/shell radius.
    // Protect only the numerical grazing singularity, not a quality tolerance.
    float areaCorrection = u_Chart.x / (u_Chart.x + height * reach);
    float projectionWeight = length(cross(tangent, ray)) / max(abs(dot(normal, ray)), 1e-4) * areaCorrection;
    float bandAlpha = 1.0 - pow(max(0.0, 1.0 - occupancy.x), 32.0 / u_Layers.x * projectionWeight);
    float bandThreshold = oloGroomStochasticHash(uint(gl_FragCoord.x), uint(gl_FragCoord.y), uint(u_ViewportFrame.z), layer + uint(u_Layers.w) * 32u, 1246u);
    float capThreshold = oloGroomStochasticHash(uint(gl_FragCoord.x), uint(gl_FragCoord.y), uint(u_ViewportFrame.z), uint(u_Layers.w), 1611u);
    if (bandAlpha <= bandThreshold && occupancy.y <= capThreshold) discard;
    float cosine = dot(tangent, normalize(vec3(0.4, 0.8, 0.3)));
    float radiance = 0.2 + 0.8 * sqrt(max(0.0, 1.0 - cosine * cosine));
    vec4 clip = u_VP * vec4(position, 1.0);
    gl_FragDepth = clip.z / clip.w * 0.5 + 0.5;
    o_Metrics = vec4(1.0, radiance, radiance, 1.0);
}

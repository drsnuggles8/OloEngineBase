#type vertex
#version 460 core
#ifdef OLO_VULKAN
layout(std430, binding = 57) readonly buffer OloVertexPull
{
    float Data[];
}
u_VertexPull;
layout(location = 0) out vec2 v_TexCoord;
void main()
{
    uint base = uint(gl_VertexIndex) * 5u;
    gl_Position = vec4(u_VertexPull.Data[base], u_VertexPull.Data[base + 1u], u_VertexPull.Data[base + 2u], 1);
    v_TexCoord = vec2(u_VertexPull.Data[base + 3u], u_VertexPull.Data[base + 4u]);
}
#else
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;
layout(location = 0) out vec2 v_TexCoord;
void main()
{
    v_TexCoord = a_TexCoord;
    gl_Position = vec4(a_Position, 1);
}
#endif
#type fragment
#version 460 core
// VulkanShader honors this per-shader request: offline exhaustive inlining
// duplicates the inverse-support graph excessively. Driver lowering remains.
#pragma optimize(off)
#extension GL_EXT_control_flow_attributes : require
#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_buffer_reference_uvec2 : require
#extension GL_EXT_descriptor_heap : require
#extension GL_EXT_nonuniform_qualifier : require
layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 o_Radiance;
layout(location = 1) out vec4 o_Raw;
layout(location = 2) out vec4 o_Diagnostics;
layout(location = 3) out vec4 o_Moments;
#include "include/BindlessHeap.glsl"
#ifdef OLO_BINDLESS
#define u_DepthTexture OLO_HEAP_TEX_2D(19)
#define u_GBufferAlbedo OLO_HEAP_TEX_2D(43)
#define u_GBufferNormal OLO_HEAP_TEX_2D(44)
#define u_GBufferEmissive OLO_HEAP_TEX_2D(45)
#define u_GVelocity OLO_HEAP_TEX_2D(46)
#define u_EnvironmentCube OLO_HEAP_TEX_CUBE(11)
#else
layout(binding = 19) uniform sampler2D u_DepthTexture;
layout(binding = 43) uniform sampler2D u_GBufferAlbedo;
layout(binding = 44) uniform sampler2D u_GBufferNormal;
layout(binding = 45) uniform sampler2D u_GBufferEmissive;
layout(binding = 46) uniform sampler2D u_GVelocity;
layout(binding = 11) uniform samplerCube u_EnvironmentCube;
#endif
#include "include/PBRCommon.glsl"
#include "include/SkyDepth.glsl"
#include "include/ReSTIRPTTypes.glsl"
#include "include/GPUScene.glsl"
#include "include/GPUSceneInstances.glsl"
#include "include/GPUSceneGeometries.glsl"
#include "include/GPUSceneMaterials.glsl"
#include "include/GPUSceneLights.glsl"
#include "include/PathTracerSampler.glsl"
#include "include/DescriptorHeapTextures.glsl"
#define OLO_RT_HIT_TLAS_ADDRESS u_TlasAddressAndFrame.xy
#define OLO_RT_HIT_INSTANCE_MASK u_TlasAddressAndFrame.z
#define OLO_RT_HIT_SLOT_COUNTS u_SlotCounts
#define OLO_RT_HIT_MATERIAL_TABLE u_MaterialTable.xy
#define OLO_RT_HIT_MATERIAL_COUNT u_MaterialTable.z
#define OLO_RT_HIT_SAMPLER_HEAP u_MaterialTable.w
#define OLO_RT_HIT_TEXTURES_REQUESTED ((u_EmissiveTable.w & OLO_RESTIR_PT_FLAG_TEXTURES) != 0u)
#include "include/RayTracedSurfaceHit.glsl"
#define PtHit OloRtHit
#define OLO_PT_CLOSURE_V2 OLO_RT_CLOSURE_V2
#include "include/PathTracerBSDF.glsl"
#include "include/GBufferRaySurface.glsl"
#include "include/ReSTIRPTCharts.glsl"
#include "include/ReSTIRPTTransport.glsl"
#include "include/ReSTIRPTShift.glsl"
#include "include/ReSTIRPTResample.glsl"
PTVertex PTPrimary(OloGBufferSurface s, uint pixel)
{
    PTVertex v = PTEmptyVertex();
    v.PositionRoughness = vec4(s.Position, s.Roughness);
    v.GeometricNormalMetallic = vec4(s.GeometricNormal, s.Metallic);
    v.ShadingNormalClosure = vec4(s.ShadingNormal, float(s.PbrModel));
    v.Albedo = vec4(s.Albedo, 1);
    v.Incoming = vec4(normalize(u_InvView[3].xyz - s.Position), 0);
    // GBuffer has no triangle identity. Primary identity denotes the source
    // receiver record, not a fictitious triangle in the acceleration structure.
    v.Identity = uvec4(0xffffffffu, pixel, 0xffffffffu, 0xffffffffu);
    return v;
}
void main()
{
    o_Radiance = vec4(0);
    o_Raw = vec4(0);
    o_Diagnostics = vec4(0);
    o_Moments = vec4(0);
    // Fail before touching a potentially incompatible storage-buffer stride.
    if (!PTFinite(u_Debug.w) || abs(u_Debug.w - float(OLO_RESTIR_PT_LAYOUT_VERSION)) > 0.0)
        return;
    ivec2 extent = ivec2(u_Screen.xy);
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    if (any(lessThan(pixel, ivec2(0))) || any(greaterThanEqual(pixel, extent)))
        return;
    uint index = uint(pixel.y) * uint(extent.x) + uint(pixel.x);
    bool hasOutput = (u_DestinationAddresses.x | u_DestinationAddresses.y) != 0u;
    if (!hasOutput)
        return;
    PTPath result = PTEmpty();
    if ((u_TlasAddressAndFrame.x | u_TlasAddressAndFrame.y) == 0u)
    {
        PTPool(u_DestinationAddresses.xy).Records[index] = result;
        return;
    }
    uint seed = oloPtMakePixelSeed(uint(pixel.x), uint(pixel.y), uint(u_Debug.y));
    OloPathSampler pathSampler = oloPtMakeSampler(seed, u_TlasAddressAndFrame.w * 4u + u_Counts.y);
    if (u_Counts.y == 0u)
    {
        float depth = texture(u_DepthTexture, v_TexCoord).r;
        vec4 emissive = texture(u_GBufferEmissive, v_TexCoord);
        int flags = oloDecodeGBufferFlags(emissive.a);
        if (!oloDepthIsSky(depth) && !oloGBufferFlagsAreUnlit(flags))
        {
            OloGBufferSurface surface = OloLoadGBufferSurface(v_TexCoord, depth, texture(u_GBufferNormal, v_TexCoord),
                                                              texture(u_GBufferAlbedo, v_TexCoord), oloGBufferFlagsPbrModel(flags));
            if (surface.Valid && PTFinite3(surface.Position))
                result = PTInitial(PTPrimary(surface, index), index, pathSampler);
        }
    }
    else
    {
        result = PTPool(u_SourceAddresses.xy).Records[index];
        if (!PTLayoutCompatible(result))
        {
            PTCount(9u);
            PTPool(u_DestinationAddresses.xy).Records[index] = PTEmpty();
            return;
        }
        bool reuse = (u_Counts.y == 1u && u_Reuse.x != 0u && u_Reuse.z != 0u) || (u_Counts.y == 2u && u_Reuse.y != 0u);
        if (reuse && PTReceiverValid(result))
        {
            ivec2 neighbourPixel = pixel;
            uvec2 address = u_SourceAddresses.zw;
            if (u_Counts.y == 1u)
            {
                vec2 uv = v_TexCoord - texture(u_GVelocity, v_TexCoord).xy;
                neighbourPixel = ivec2(floor(uv * vec2(extent)));
                address = u_HistoryAddresses.xy;
            }
            else
            {
                float angle = oloPtGet1D(pathSampler) * TWO_PI;
                float radius = max(1.0, u_EstimatorParams.w * sqrt(oloPtGet1D(pathSampler)));
                neighbourPixel += ivec2(round(vec2(cos(angle), sin(angle)) * radius));
            }
            if ((address.x | address.y) != 0u && all(greaterThanEqual(neighbourPixel, ivec2(0))) && all(lessThan(neighbourPixel, extent)))
            {
                uint neighbourIndex = uint(neighbourPixel.y) * uint(extent.x) + uint(neighbourPixel.x);
                PTPath neighbour = PTPool(address).Records[neighbourIndex];
                bool age = u_TlasAddressAndFrame.w >= neighbour.Lineage.x && u_TlasAddressAndFrame.w - neighbour.Lineage.x <= 1u;
                if (PTReceiverValid(neighbour) && age && neighbour.Metadata.z == u_Counts.w && neighbour.Metadata.w == u_Reuse.w &&
                    PTCompatibleReceiver(result.Vertices[0], neighbour.Vertices[0]))
                    result = PTCombine(result, neighbour, u_Counts.z & 7u, pathSampler);
                else
                    PTCount(9u);
            }
        }
    }
    PTPool(u_DestinationAddresses.xy).Records[index] = result;
    if (!PTReceiverValid(result))
        return;
    vec3 value = PTSelected(result) ? result.Value.rgb * result.State.x : vec3(0);
    PTPath raw = result;
    if (u_Counts.y == 3u && (u_HistoryAddresses.z | u_HistoryAddresses.w) != 0u)
        raw = PTPool(u_HistoryAddresses.zw).Records[index];
    if (!PTLayoutCompatible(raw))
    {
        PTCount(9u);
        raw = PTEmpty();
    }
    vec3 rawValue = PTSelected(raw) ? raw.Value.rgb * raw.State.x : vec3(0);
    float clamped = 0.0;
    if (u_EstimatorParams.z > 0.0)
    {
        clamped = any(greaterThan(value, vec3(u_EstimatorParams.z))) ? 1.0 : 0.0;
        value = min(value, vec3(u_EstimatorParams.z));
    }
    if (!PTFinite3(value) || !PTFinite3(rawValue))
    {
        PTCount(13u);
        return;
    }
    if (u_Counts.y == 3u)
    {
        PTCount(14u);
        if (clamped > 0.0)
            PTCount(15u);
    }
    o_Raw = vec4(rawValue, 1);
    o_Diagnostics = vec4(PTSelected(result) ? 1.0 : 0.0, float(result.Lineage.z), result.State.w, clamped);
    float rawLuminance = dot(rawValue, vec3(0.2126, 0.7152, 0.0722));
    o_Moments = vec4(rawLuminance, raw.Value.w, raw.State.y, 1);
    uint view = uint(u_Debug.x);
    if (view == 1u)
        value = rawValue;
    // History view describes the selected suffix, not merely whether a fresh
    // reservoir happened to contain a valid sample. Diagnostics.x retains
    // its independent selected-valid contract for existing readback tools.
    else if (view == 2u)
        value = (result.Lineage.w & OLO_RESTIR_PT_LINEAGE_HISTORY) != 0u ? vec3(0, 1, 0) : vec3(1, 0, 0);
    else if (view == 3u)
        value = vec3(raw.Value.w / (1.0 + raw.Value.w));
    else if (view == 4u)
        value = vec3(float(result.Lineage.z) / 4.0, float(u_TlasAddressAndFrame.w - result.Lineage.x), float(result.Lineage.y % 31u) / 31.0);
    else if (view == 5u)
        value = vec3(max(result.State.w, 0.0), max(-result.State.w, 0.0), 0) / log(8.0);
    else if (view == 6u)
        value = vec3(clamped, u_EstimatorParams.z > 0.0 ? 1.0 : 0.0, 0);
    o_Radiance = vec4(value, 1);
}

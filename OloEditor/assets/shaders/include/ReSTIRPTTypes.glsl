#ifndef OLO_RESTIR_PT_TYPES
#define OLO_RESTIR_PT_TYPES
#define OLO_RESTIR_PT_FLAG_TEXTURES 1u
#define OLO_RESTIR_PT_FLAG_ENVIRONMENT 2u
#define OLO_RESTIR_PT_LAYOUT_VERSION 1u
#define OLO_RESTIR_PT_LAYOUT_BITS (OLO_RESTIR_PT_LAYOUT_VERSION << 16u)
// Lineage.w: bit0 receiver exists, bit1 selected nonzero-target path exists,
// bit2 the selected suffix came through temporal reuse. Spatial reuse carries
// that ancestry; a fresh initial candidate never sets it. Admission of a
// temporal candidate that loses reservoir selection does not set this bit.
// Bits16..31 carry the record layout version. CPU/GPU ABI changes require a
// matching engine rebuild and editor restart; history invalidation alone does
// not change buffer strides or the CPU parameter layout. Debug.w carries the
// CPU version, checked before any record buffer access.
#define OLO_RESTIR_PT_LINEAGE_HISTORY 4u
// CPU mirror: Renderer/ReSTIR/ReSTIRPTGPU.h. std430 strides 112/80/592.
struct PTVertex
{
    vec4 PositionRoughness;
    vec4 GeometricNormalMetallic;
    vec4 ShadingNormalClosure;
    vec4 Albedo;
    vec4 Incoming;
    vec4 Randoms;
    uvec4 Identity;
};
struct PTEndpoint
{
    vec4 PositionKind;
    vec4 NormalTwoSided;
    vec4 RadianceDensity;
    vec4 Randoms;
    uvec4 Identity;
};
struct PTPath
{
    PTVertex Vertices[4];
    PTEndpoint Endpoint;
    uvec4 Metadata;
    uvec4 Lineage;
    vec4 State;
    vec4 Value;
};
layout(buffer_reference, std430, buffer_reference_align = 16) buffer PTPool
{
    PTPath Records[];
};
layout(buffer_reference, std430, buffer_reference_align = 16) buffer PTCounters
{
    uint Values[16];
};
layout(std140, binding = 65) uniform ReSTIRPTParams
{
    mat4 u_InvView;
    mat4 u_InvProjection;
    mat4 u_View;
    uvec4 u_TlasAddressAndFrame;
    uvec4 u_SlotCounts;
    uvec4 u_EmissiveTable;
    uvec4 u_MaterialTable;
    uvec4 u_SourceAddresses;
    uvec4 u_DestinationAddresses;
    uvec4 u_HistoryAddresses;
    uvec4 u_Counts;
    vec4 u_Params;
    vec4 u_EstimatorParams;
    vec4 u_Environment;
    vec4 u_Screen;
    uvec4 u_Reuse;
    vec4 u_Debug;
};
void PTCount(uint index)
{
    if ((u_DestinationAddresses.z | u_DestinationAddresses.w) != 0u)
        atomicAdd(PTCounters(u_DestinationAddresses.zw).Values[index], 1u);
}
bool PTFinite(float x)
{
    return !isnan(x) && !isinf(x);
}
bool PTFinite3(vec3 x)
{
    return !any(isnan(x)) && !any(isinf(x));
}
bool PTOpen(float x)
{
    return PTFinite(x) && x > 0.0 && x < 1.0;
}
bool PTSelected(PTPath p)
{
    return (p.Lineage.w >> 16u) == OLO_RESTIR_PT_LAYOUT_VERSION &&
           (p.Lineage.w & 2u) != 0u && p.State.x > 0.0 && p.State.z > 0.0;
}
bool PTLayoutCompatible(PTPath p)
{
    return (p.Lineage.w >> 16u) == OLO_RESTIR_PT_LAYOUT_VERSION;
}
bool PTReceiverValid(PTPath p)
{
    return PTLayoutCompatible(p) && (p.Lineage.w & 1u) != 0u;
}
PTVertex PTEmptyVertex()
{
    PTVertex v;
    v.PositionRoughness = vec4(0);
    v.GeometricNormalMetallic = vec4(0);
    v.ShadingNormalClosure = vec4(0);
    v.Albedo = vec4(0);
    v.Incoming = vec4(0);
    v.Randoms = vec4(0);
    v.Identity = uvec4(0xffffffffu);
    return v;
}
PTPath PTEmpty()
{
    PTPath p;
    for (int k = 0; k < 4; ++k)
        p.Vertices[k] = PTEmptyVertex();
    p.Endpoint.PositionKind = vec4(0);
    p.Endpoint.NormalTwoSided = vec4(0);
    p.Endpoint.RadianceDensity = vec4(0);
    p.Endpoint.Randoms = vec4(0);
    p.Endpoint.Identity = uvec4(0xffffffffu);
    p.Metadata = uvec4(0, 0, u_Counts.w, u_Reuse.w);
    p.Lineage = uvec4(0, 0, 0, OLO_RESTIR_PT_LAYOUT_BITS);
    p.State = vec4(0);
    p.Value = vec4(0);
    return p;
}
#endif

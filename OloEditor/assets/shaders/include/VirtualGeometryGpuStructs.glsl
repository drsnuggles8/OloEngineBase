#ifndef VIRTUAL_GEOMETRY_GPU_STRUCTS_GLSL
#define VIRTUAL_GEOMETRY_GPU_STRUCTS_GLSL

// =============================================================================
// VirtualGeometryGpuStructs.glsl — the ONE GLSL spelling of the std430 mirrors
// in OloEngine/Renderer/VirtualGeometry/VirtualMeshGpuData.h (issues #629/#813).
//
// Struct DEFINITIONS only, deliberately no buffer/uniform block declarations:
// which bindings a stage declares (and with which memory qualifiers) is the
// stage's own business, and an include that declared blocks would change every
// includer's reflected binding set. Consumers declare e.g.
//   layout(std430, binding = 33) readonly buffer VirtualClusters { VirtualCluster clusters[]; };
//
// The pre-#813 shaders (VirtualClusterCull.comp, VirtualClusterRaster.comp,
// VirtualVisibilityResolve.glsl, VirtualMeshShadowDepth.glsl) still carry
// local copies of these structs — folding them onto this header is recorded
// follow-up work; anything NEW must include this instead of re-declaring.
// =============================================================================

// Mesh-shader raster limits — MUST mirror kMeshletMaxVertices /
// kMeshletMaxTriangles in VirtualMeshGpuData.h (pinned by
// VirtualMeshletEligibility.ShaderLayoutConstantsMatchTheCppLimits, which
// parses this file). One mesh workgroup renders one cluster; a cluster the
// cook let exceed these routes to the MDI path instead.
#define OLO_MESHLET_MAX_VERTICES 128
#define OLO_MESHLET_MAX_PRIMITIVES 128

// Mirrors OloEngine::VirtualGpuVertex (32 B)
struct VirtualGpuVertex {
    vec4 PositionU; // xyz mesh-local position, w = TexCoord.x
    vec4 NormalV;   // xyz mesh-local normal,   w = TexCoord.y
};

// Mirrors OloEngine::VirtualClusterGpuRecord (64 B). VertexCount is the former
// _Pad1 slot (#813): the pack writes it for the mesh-shader path's
// SetMeshOutputsEXT; layout-identical for older shaders that still spell the
// slot as padding.
struct VirtualCluster {
    vec4 CullSphere;   // xyz mesh-local center, w radius
    vec4 Cone;         // xyz mesh-local axis, w cutoff (>= 1 disables)
    uint VertexBase;
    uint IndexBase;
    uint IndexCount;
    uint GroupIndex;
    uint RefinedGroup; // ~0u for LOD-0 clusters
    uint Lod;
    uint VertexCount;  // cluster-owned vertex window (<= OLO_MESHLET_MAX_VERTICES on the mesh path)
    uint _Pad2;
};

// Mirrors OloEngine::VirtualInstanceGpuRecord (224 B)
struct VirtualInstance {
    mat4 Transform;      // render-origin-relative
    mat4 PrevTransform;
    mat4 NormalMatrix;
    uint ClusterBase;
    uint ClusterCount;
    uint GroupBase;
    int  EntityID;
    float MaxScale;
    float ErrorThresholdPixels;
    uint CommandBase;
    uint Flags;
};

// Mirrors OloEngine::VirtualVisibleCluster (16 B)
struct VisibleCluster {
    uint InstanceIndex;
    uint ClusterIndex;
    uint _Pad0;
    uint _Pad1;
};

// Mirrors OloEngine::VirtualDrawArgs (16 B stride — the MDI parameter-buffer
// word; the mesh path's task stage reads DrawCount as its launch count)
struct VirtualDrawArgs {
    uint DrawCount;
    uint TestedCount;
    uint CutSelected;
    uint SwCount;
};

#endif // VIRTUAL_GEOMETRY_GPU_STRUCTS_GLSL

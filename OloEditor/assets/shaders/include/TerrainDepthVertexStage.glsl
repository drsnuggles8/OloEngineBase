// THE GLSL TEXT ROUTE ON OPENGL (#1533): this program reaches a GL driver as
// SPIRV-Cross GLSL text, not glShaderBinary SPIR-V, because NVIDIA's GLSL front
// end runs the lawn and the terrain several times faster than its SPIR-V
// ingestion. Vulkan is unaffected. docs/agent-rules/gl-shader-route.md.
#define OLO_GL_GLSL_ROUTE 1

#ifdef OLO_VULKAN
// #691 (ADR 0011 §5, amendment (76)): vertex pull from the engine-wide
// binding 57. Draw site is the terrain patch VBO (TerrainChunk.cpp /
// TerrainVertex.h — 32 B: vec3 Position @0, vec2 TexCoord @12, vec3 Normal
// @20), so the stride is 8 floats but the FIELD ORDER differs from the
// engine `Vertex` (uv before normal); the normal is unused by this depth
// stage. Only the vertex stage pulls; the tess control/eval stages read the
// vertex stage's outputs unchanged. The GL attribute branch below is
// untouched.
layout(std430, binding = 57) readonly buffer OloVertexPull
{
    float v[];
} b_Vertices;
#define OLO_PULLED_VERTEX 1
#else
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;
layout(location = 2) in vec3 a_Normal;
#endif

// GPU-driven LOD (issue #714). The shadow-caster draws keep the chunk meshes
// and set u_TerrainGpuDriven = 0, so this is inert for them — it exists so the
// depth program stays usable if the shadow path ever moves onto the node list.
#include "TerrainGpuDrivenVertex.glsl"

layout(location = 0) out vec3 v_Position;
layout(location = 1) out vec2 v_TexCoord;

void main()
{
#ifdef OLO_PULLED_VERTEX
    int vertBase = gl_VertexIndex * 8;
    vec3 a_Position = vec3(b_Vertices.v[vertBase + 0], b_Vertices.v[vertBase + 1], b_Vertices.v[vertBase + 2]);
    vec2 a_TexCoord = vec2(b_Vertices.v[vertBase + 3], b_Vertices.v[vertBase + 4]);
#endif
    vec3 position = a_Position;
    vec2 texCoord = a_TexCoord;
    vec3 normal = vec3(0.0, 1.0, 0.0); // unused by this stage; the helper wants a slot
    oloTerrainApplyGpuDrivenNode(position, texCoord, normal);

    v_Position = position;
    v_TexCoord = texCoord;
}

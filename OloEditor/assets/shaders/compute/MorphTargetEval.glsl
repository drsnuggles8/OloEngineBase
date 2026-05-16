#version 460 core
layout(local_size_x = 256) in;

struct Vertex
{
    vec3 position;
    float _pad0;
    vec3 normal;
    float _pad1;
};

struct MorphDelta
{
    vec3 deltaPos;
    float _pad0;
    vec3 deltaNormal;
    float _pad1;
};

layout(std430, binding = 0) readonly buffer BaseVertices { Vertex baseVerts[]; };
layout(std430, binding = 1) readonly buffer MorphDeltas  { MorphDelta deltas[]; };  // [target * vertexCount + vertex]
layout(std430, binding = 2) readonly buffer Weights      { float weights[]; };
layout(std430, binding = 3) writeonly buffer OutputVerts  { Vertex outVerts[]; };

// Vertex / target counts are derived from the bound SSBO sizes via the
// GLSL 4.30+ `.length()` builtin. The earlier revision pushed them through
// a small UBO at binding 0 — but binding 0 is reserved for UBO_CAMERA at
// the OpenGL level (a program-global indexed binding pool), so reading it
// from a compute shader yielded whatever the last graphics pass had bound
// there. Using `.length()` keeps this compute shader self-contained and
// removes the silent collision.
void main()
{
    uint vid = gl_GlobalInvocationID.x;
    uint vertexCount = baseVerts.length();
    if (vid >= vertexCount)
        return;

    uint targetCount = weights.length();

    vec3 pos = baseVerts[vid].position;
    vec3 nrm = baseVerts[vid].normal;

    for (uint t = 0; t < targetCount; ++t)
    {
        float w = weights[t];
        if (w > 0.001)
        {
            uint idx = t * vertexCount + vid;
            pos += deltas[idx].deltaPos * w;
            nrm += deltas[idx].deltaNormal * w;
        }
    }

    outVerts[vid].position = pos;
    outVerts[vid].normal = normalize(nrm);
}

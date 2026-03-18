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
layout(std430, binding = 1) readonly buffer MorphDeltas  { MorphDelta deltas[]; };  // [target][vertex]
layout(std430, binding = 2) readonly buffer Weights      { float weights[]; };
layout(std430, binding = 3) writeonly buffer OutputVerts  { Vertex outVerts[]; };

uniform uint u_VertexCount;
uniform uint u_TargetCount;

void main()
{
    uint vid = gl_GlobalInvocationID.x;
    if (vid >= u_VertexCount)
        return;

    vec3 pos = baseVerts[vid].position;
    vec3 nrm = baseVerts[vid].normal;

    for (uint t = 0; t < u_TargetCount; ++t)
    {
        float w = weights[t];
        if (w > 0.001)
        {
            uint idx = t * u_VertexCount + vid;
            pos += deltas[idx].deltaPos * w;
            nrm += deltas[idx].deltaNormal * w;
        }
    }

    outVerts[vid].position = pos;
    outVerts[vid].normal = normalize(nrm);
}

// =============================================================================
// UploadOrderingProbe.glsl — issue #1351.
//
// One draw observes all three CPU-uploaded buffer families at once, so a test
// can rewrite ONE of them between draws of the same object and read back what
// each recorded draw actually saw:
//
//   * the VERTEX stream: position (xy) and a colour pair (zw) per vertex,
//     pulled through SSBO 57 on Vulkan and read as attributes on GL;
//   * the UNIFORM block at 18: two vec4s, A and B;
//   * the STORAGE block at 15: an array of vec4 entries, of which [0] and [1]
//     are read.
//
// The output is the SUM of the three families' contributions, channel by
// channel:
//
//   r = vertex.z + A.x + entries[0].x
//   g = vertex.w + B.x + entries[1].x
//
// A test holds the two families it is not exercising at zero, so each channel
// reads back one family's bytes. A and B (and entries[0] / entries[1]) sit in
// DIFFERENT 16-byte ranges on purpose: a partial write of one leaves the other
// in bytes the write does not cover, which is the case a snapshot sized to the
// write — rather than the buffer — gets wrong.
//
// Binding choice. 18 is the ShaderUnit / MaterialLab test-probe UBO slot (its
// production occupant, UBO_PRECIPITATION, never runs during a probe draw). 15
// is SSBO_INSTANCE_DATA, the binding the #691 interleaved-upload tenant used;
// this shader declares its own block there because it never includes
// InstanceBlock_Vertex.glsl. Test shaders are outside the production binding
// scan (ShaderHarness skips tests/).
//
// Kept in step with the vertex layout and constants in
// OloEngine/tests/Rendering/VulkanPassSuiteTest.cpp.
// =============================================================================

#type vertex
#version 450 core

#ifdef OLO_VULKAN
// ADR 0011 §5: no vertex-input state on Vulkan; stream 0 of the draw's VAO is
// reachable through the reserved pull binding. Stride = 4 floats.
layout(std430, binding = 57) readonly buffer OloVertexPull
{
    float v[];
} b_Vertices;
#else
layout(location = 0) in vec2 a_Position;
layout(location = 1) in vec2 a_Colour;
#endif

layout(location = 0) out vec2 v_Colour;

void main()
{
#ifdef OLO_VULKAN
    int base = gl_VertexIndex * 4;
    vec2 a_Position = vec2(b_Vertices.v[base + 0], b_Vertices.v[base + 1]);
    vec2 a_Colour = vec2(b_Vertices.v[base + 2], b_Vertices.v[base + 3]);
#endif
    v_Colour = a_Colour;
    gl_Position = vec4(a_Position, 0.0, 1.0);
}

#type fragment
#version 450 core

layout(location = 0) in vec2 v_Colour;

layout(location = 0) out vec4 o_Colour;

layout(std140, binding = 18) uniform UploadProbeParams
{
    vec4 u_A;
    vec4 u_B;
};

layout(std430, binding = 15) readonly buffer UploadProbeEntries
{
    vec4 b_Entries[];
};

void main()
{
    float r = v_Colour.x + u_A.x + b_Entries[0].x;
    float g = v_Colour.y + u_B.x + b_Entries[1].x;
    o_Colour = vec4(r, g, 0.0, 1.0);
}

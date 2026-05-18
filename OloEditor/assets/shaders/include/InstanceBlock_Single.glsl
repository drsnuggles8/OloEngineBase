#ifndef INSTANCE_BLOCK_SINGLE_GLSL
#define INSTANCE_BLOCK_SINGLE_GLSL

// Single-instance variant of the instance SSBO macros — reads `instances[0]`
// directly, no `gl_InstanceIndex` / `v_InstanceIndex`. Use this in:
//   - tess_control / tess_evaluation / geometry stages where vertex inputs are
//     arrays and the regular `flat in int v_InstanceIndex` declaration fails
//     to compile.
//   - compute shaders that produce instance data but don't draw it.
//   - any shader stage that genuinely only ever sees one instance per draw
//     (terrain, water, full-screen passes).
//
// The auto-batching path NEVER routes a multi-instance draw to a shader that
// only uses this include — tessellated meshes are single-instance by design.
struct InstanceData {
    mat4 Transform;
    mat4 Normal;
    mat4 PrevTransform;
    vec4 Color;
    int  EntityID;
    float Custom;
    int  _instancePad0;
    int  _instancePad1;
};

layout(std430, binding = 15) readonly buffer InstanceBuffer {
    InstanceData instances[];
};

#define u_Model        (instances[0].Transform)
#define u_Normal       (instances[0].Normal)
#define u_PrevModel    (instances[0].PrevTransform)
#define u_EntityID     (instances[0].EntityID)
#define u_NormalMatrix (instances[0].Normal)

#endif // INSTANCE_BLOCK_SINGLE_GLSL

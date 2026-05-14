// =============================================================================
// SkyboxSampling.glsl
//
// Shared cubemap sampling convention for visible skybox shaders and tests.
// OloEngine uploads cubemap faces with stb_image's native row order
// (top image row first). With OpenGL's cubemap face-coordinate convention,
// the unmodified direction keeps screen-space up sampling the upper image rows
// of side faces and keeps +Y/-Y mapped to top/bottom faces respectively.
// =============================================================================
#ifndef OLO_SKYBOX_SAMPLING_GLSL
#define OLO_SKYBOX_SAMPLING_GLSL

vec3 GetSkyboxSampleDirection(vec3 direction)
{
    return direction;
}

#endif // OLO_SKYBOX_SAMPLING_GLSL

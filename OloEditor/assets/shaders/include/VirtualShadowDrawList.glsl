#ifndef OLO_VIRTUAL_SHADOW_DRAWLIST_GLSL
#define OLO_VIRTUAL_SHADOW_DRAWLIST_GLSL

// =============================================================================
// VirtualShadowDrawList.glsl — the cull -> raster handoff (issue #702)
//
// VSM_CullCasters.comp writes these records; the VSM depth raster's vertex stage
// reads them. Two files, one declaration, so the layout cannot drift between the
// producer and the consumer.
//
// The CLIP LEVEL lives in the record rather than in a per-draw uniform, and that
// is what collapses the draw count: one glDrawElementsIndirect per caster batch
// covers all VSM_CLIP_LEVELS levels, instead of sixteen draws per batch.
//
// C++ twin: VSM::DrawInstance.
// =============================================================================

struct VSMDrawInstance
{
    mat4 Transform; // render-relative model matrix
    uint ClipLevel;
    uint _pad0;
    uint _pad1;
    uint _pad2;
};

layout(std430, binding = 66) buffer VSMDrawInstances { VSMDrawInstance b_DrawInstances[]; };

#endif // OLO_VIRTUAL_SHADOW_DRAWLIST_GLSL

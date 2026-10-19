// The fragment stage of VSM_GroomDepth.glsl and VSM_GroomDepthCrossFade.glsl:
// VirtualShadowRasterStage.glsl's write into a clip level, or
// VirtualShadowLocalRasterStage.glsl's into a local-light layer (#1592), plus the LOD cross-fade's mask (#1612)
// where OLO_GROOM_CROSSFADE_MASK is defined -- the *CrossFade.glsl variant, bound
// for a fading caster alone -- so every other caster keeps a discard-free
// fragment stage.
//
// EVERY INCLUDE IS UNCONDITIONAL, and that is load-bearing. The engine's include
// expander (OpenGLShader::ProcessIncludesInternal) pastes text and skips a file
// it has already pasted, without evaluating #ifdef. An include inside a dead
// branch therefore still claims the file, and the same include in the live
// branch is then skipped. glslc evaluates the conditional and passes the
// shader the engine cannot compile. So the files are taken once, here, and only
// the mask itself is conditional; VirtualShadowRasterStage.glsl's main() is
// restated below rather than included, because it cannot discard.
#include "GroomStrandCommon.glsl"
#include "VirtualShadowResources.glsl"
#include "VirtualShadowRasterWrite.glsl"
#include "VirtualShadowLocalRasterWrite.glsl"

// x = domain (0 clip level, 1 local-light layer, #1592), y = level or layer,
// z = the layer's raster mip.
layout(location = 0) flat in uvec3 v_VSMTarget;
#ifdef OLO_GROOM_CROSSFADE_MASK
layout(location = 1) flat in vec4 v_CrossFade;
layout(location = 2) flat in uint v_CrossFadeFrame;
#endif

void main()
{
#ifdef OLO_GROOM_CROSSFADE_MASK
	// Per VIRTUAL texel: the same texel both tiers rasterise into.
	if (!oloGroomCrossFadeKeeps(uint(floor(gl_FragCoord.x)), uint(floor(gl_FragCoord.y)), v_CrossFadeFrame,
	                            v_CrossFade))
	{
		discard;
	}
#endif
	if (v_VSMTarget.x == 1u)
	{
		vsmWriteLocalRasterDepth(v_VSMTarget.y, v_VSMTarget.z);
	}
	else
	{
		vsmWriteRasterDepth(v_VSMTarget.y);
	}
}

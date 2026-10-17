// The fragment stage of GroomStrandDepth.glsl and GroomStrandDepthCrossFade.glsl.
// The LOD cross-fade's mask (#1612) is compiled in only where
// OLO_GROOM_CROSSFADE_MASK is defined -- the *CrossFade.glsl variant, bound for a
// fading caster alone -- so every other caster keeps an empty (or discard-free)
// fragment stage and the driver's early depth test.
//
// The include is unconditional: the engine's include expander pastes text and
// does not evaluate #ifdef (see include/VSM_GroomDepthFragment.glsl). Unused
// functions cost the steady caster nothing.
#include "GroomStrandCommon.glsl"

#ifdef OLO_GROOM_CROSSFADE_MASK
layout(location = 0) flat in vec4 v_CrossFade;
layout(location = 1) flat in uint v_CrossFadeFrame;
#endif

void main()
{
	// Depth is written automatically by the rasterizer.
#ifdef OLO_GROOM_CROSSFADE_MASK
	// The cross-fade mask per SHADOW TEXEL (#1612): both tiers ask the same
	// texel, so the pair casts each texel once and the coat's shadow fades with
	// its colour.
	if (!oloGroomCrossFadeKeeps(uint(floor(gl_FragCoord.x)), uint(floor(gl_FragCoord.y)), v_CrossFadeFrame,
	                            v_CrossFade))
	{
		discard;
	}
#endif
}

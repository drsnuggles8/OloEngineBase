#ifndef OLO_SURFACE_COVERAGE_MARK_GLSL
#define OLO_SURFACE_COVERAGE_MARK_GLSL

// RT3 .a, the material profile, holds this where the pixel's coverage (.b) is
// one draw of a STOCHASTIC estimator (#1552): a groom under StochasticAlpha,
// which has no profile of its own. A profile is never negative, so the mark
// cannot pass for one, and a reader of the profile takes max(.a, 0).
//
// TAA keeps such a pixel's history weight in motion: its current frame is
// noise, where an alpha-tested leaf's, which also writes fractional coverage,
// is exact and keeps the motion ramp.
#define OLO_STOCHASTIC_COVERAGE_MARK (-1.0)

#endif // OLO_SURFACE_COVERAGE_MARK_GLSL

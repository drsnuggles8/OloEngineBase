// =============================================================================
// GroomCoatShadowCommon.glsl — how much coat is between this strand and the
// light. Issue #1248.
//
// TWIN OF OloEngine/Groom/GroomCoatShadow.{h,cpp}. SampleDensityVolume on the
// CPU and oloGroomCoatOpticalDepth here compute the same number the same way,
// and the order of operations is mirrored deliberately rather than just the
// formula: a mathematically equal rearrangement moves the last bits.
//
// HALF OF THIS PAIR NOW HAS A GPU PARITY TEST, and which half is stated rather
// than implied. #1360 added GroomCoatTransmittanceParityTest, which compiles
// this file and compares oloGroomCoatTransmittance against
// GroomCoatShadow::CoatTransmittance over the whole authored kappa range.
//
// THE MARCH IS STILL UNGUARDED. oloGroomCoatOpticalDepth against
// SampleDensityVolume needs a 3D volume uploaded and read back identically on
// both sides, which no probe does yet; what guards it today is the CPU
// comparison plus the visual-evidence A/B, neither of which can see the two
// sides drifting in the same direction. Do not read the mirroring of the march
// as something a test is currently checking.
//
// WHAT IT COMPUTES. tau = the expected number of fibre crossings between a
// point and the light, from which transmittance is exp(-tau * (1 - exp(-kappa)))
// — the MEAN of the per-ray transmittances rather than the transmittance of the
// mean crossing count, which is the Jensen correction #1360 made. Purely
// geometric — fibre areal density times the sine of the angle between the ray
// and the local fibre direction, integrated along the ray. It sees NO colour:
// the pigment already attenuates inside each fibre in GroomFibreCommon.glsl,
// and applying it twice is the double-count the issue's scope note forbids.
//
// THE SAMPLER IS A PARAMETER, not a declaration in this header. That is the
// FogVolumeCommon.glsl pattern (evaluateFogVolumesAtPointVDB takes its
// sampler3D as an argument) and it is what lets a shader include this file
// without being forced to declare — and therefore to bind — a volume it may not
// have. A shader that never coat-shadows pays nothing.
//
// THE VOLUME IS IN GROOM OBJECT SPACE, not world space, so a coat that merely
// MOVES reuses its bake instead of rebuilding it. That is the whole of the
// update policy for a rigid groom, and it is why the march transforms the ray
// into object space rather than baking the model matrix into the volume.
// A coat BOUND to an animating body (#1426) uses the same space and the same
// lookup: its volume is baked from the strands the pass draws, which are
// already in object space with the pose applied, and rebaked on the CPU when
// they drift past a bound. Nothing in this file distinguishes the two.
// u_GroomCoatWorldToObject must therefore be RIGID (rotation + translation
// only): the march takes angles in that space, and a scale in it would tilt
// every fibre direction by an amount that depends on which way the ray points.
// =============================================================================

#ifndef OLO_GROOM_COAT_SHADOW_COMMON_GLSL
#define OLO_GROOM_COAT_SHADOW_COMMON_GLSL

// GroomCoatShadow::CoatShadowMode. Integer constants rather than floats
// because the shader compares them exactly, and a float lane would make an
// exact comparison a rounding question.
#define OLO_GROOM_COAT_MODE_NONE 0
#define OLO_GROOM_COAT_MODE_ISOTROPIC 1
#define OLO_GROOM_COAT_MODE_ANISOTROPIC 2
#define OLO_GROOM_COAT_MODE_DEEP_OPACITY 3

// E[sin(theta)] between a ray and a uniformly oriented fibre: the integral of
// sin over the sphere divided by the solid angle, which is exactly pi/4. It is
// what turns "fibre length times diameter per unit volume" into "expected
// crossings per unit length of ray" for an uncombed coat. kIsotropicMeanSine in
// GroomCoatShadow.cpp.
const float OLO_GROOM_COAT_ISOTROPIC_MEAN_SINE = 0.78539816339744830961;

// The march is bounded by a COMPILE-TIME constant and the loop breaks on a
// uniform, rather than looping to the uniform directly. A data-dependent loop
// bound is what made an AMD Mesa driver spend minutes compiling this shader
// class (glsl-shaders.md); the fixed bound keeps the unroller's job finite.
#define OLO_GROOM_COAT_MAX_STEPS 64

// The packed volume: xyz = the voxel's mean fibre direction times its
// coherence, w = the fibre areal density in 1/metre.
//
// ONE RGBA texture rather than a separate density and direction pair, and that
// is a bandwidth decision rather than a packing convenience: the march is the
// hot loop, and two fetches per step would double its bandwidth for a channel
// the isotropic arm does not even read. It also costs ONE sampler slot, and the
// sampler namespace has exactly one index left (ShaderBindingLayout.h).
//
// RGBA32F on the wire, 16 bytes a voxel. RGBA16F would halve that and is what
// the packing wants, but Texture3D's RGBA16F declares 8 bytes a texel while
// uploading its client data as GL_FLOAT, so SetData rejects the only buffer it
// could be handed. See GroomRenderPass's bake.
struct OloGroomCoatVolumeSample
{
	vec3 Direction; // mean direction * coherence, object space
	float Density;  // fibre areal density, 1/m
};

OloGroomCoatVolumeSample oloGroomCoatSampleVolume(sampler3D coatVolume, vec3 uvw)
{
	vec4 packed_ = texture(coatVolume, uvw);
	OloGroomCoatVolumeSample result;
	result.Direction = packed_.xyz;
	result.Density = packed_.w;
	return result;
}

// The projected cross-section a voxel presents to a ray travelling `dirObject`.
//
// The isotropic arm takes the sphere average; the anisotropic arm blends
// towards the voxel's own mean direction by how coherent the fibres in it are.
// BLENDING rather than switching is what keeps a voxel holding two crossing
// strands from claiming a direction it does not have — its accumulated
// direction is short, so it falls back towards pi/4 on its own.
float oloGroomCoatMeanSine(vec3 meanDirection, vec3 dirObject, int mode)
{
	if (mode != OLO_GROOM_COAT_MODE_ANISOTROPIC)
	{
		return OLO_GROOM_COAT_ISOTROPIC_MEAN_SINE;
	}

	float coherence = clamp(length(meanDirection), 0.0, 1.0);
	if (coherence <= 1.0e-4)
	{
		return OLO_GROOM_COAT_ISOTROPIC_MEAN_SINE;
	}

	vec3 axis = meanDirection / coherence;
	float cosTheta = clamp(dot(axis, dirObject), -1.0, 1.0);
	float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
	return coherence * sinTheta + (1.0 - coherence) * OLO_GROOM_COAT_ISOTROPIC_MEAN_SINE;
}

// The ray's entry and exit parameters for the unit UVW box. A ray starting
// inside gets tEnter = 0. Returns false when the ray misses the volume
// entirely, which is the common case for a light that does not pass through
// the coat at all.
bool oloGroomCoatIntersectUnitBox(vec3 originUvw, vec3 dirUvw, out float tEnter, out float tExit)
{
	float t0 = 0.0;
	float t1 = 3.4028235e38;

	// Written branchlessly over the three slabs rather than as a loop with an
	// early return: a uniform-trip loop of three is free to unroll and the
	// divide is guarded by the sign test rather than by a branch.
	for (int axis = 0; axis < 3; ++axis)
	{
		float d = dirUvw[axis];
		float o = originUvw[axis];
		if (abs(d) < 1.0e-12)
		{
			if (o < 0.0 || o > 1.0)
			{
				return false;
			}
			continue;
		}
		float inv = 1.0 / d;
		// NOT named near / far: those are Win32 macros, and while GLSL does not
		// care, the C++ twin does and the two are read side by side.
		float slabEnter = (0.0 - o) * inv;
		float slabExit = (1.0 - o) * inv;
		if (slabEnter > slabExit)
		{
			float swap = slabEnter;
			slabEnter = slabExit;
			slabExit = swap;
		}
		t0 = max(t0, slabEnter);
		t1 = min(t1, slabExit);
		if (t0 > t1)
		{
			return false;
		}
	}

	tEnter = t0;
	tExit = t1;
	return true;
}

// Expected fibre crossings between `worldPos` and the light, along `L` (the
// direction TOWARDS the light, already normalised).
//
//   coatVolume      the packed RGBA32F volume
//   worldToObject   RIGID render-relative-world -> groom object space
//   boundsMin       the volume's object-space minimum corner
//   invExtent       1 / (boundsMax - boundsMin), object space
//   stepWorld       march step in WORLD METRES (the selected value is three
//                   voxels; see the analysis document's finding 2)
//   mode            OLO_GROOM_COAT_MODE_*
//
// Returns 0 for a ray that misses the volume — an UNSHADOWED answer, never a
// fully shadowed one. The failure mode of a missing lookup has to be a bright
// coat, which reads as "this did not run", rather than a black one, which is
// indistinguishable from a correct silhouette.
float oloGroomCoatOpticalDepth(sampler3D coatVolume, mat4 worldToObject, vec3 boundsMin, vec3 invExtent,
                               vec3 worldPos, vec3 L, float stepWorld, int mode)
{
	if (mode == OLO_GROOM_COAT_MODE_NONE || stepWorld <= 0.0)
	{
		return 0.0;
	}

	vec3 originObject = (worldToObject * vec4(worldPos, 1.0)).xyz;
	// The rigid rotation, so the direction keeps its angles with the fibres.
	vec3 dirObject = normalize(mat3(worldToObject) * L);

	vec3 originUvw = (originObject - boundsMin) * invExtent;
	// NOT normalised: the UVW direction has to carry the per-axis scale, or a
	// non-cubic volume would march at a different world rate on each axis.
	vec3 dirUvw = dirObject * invExtent;

	float tEnter;
	float tExit;
	if (!oloGroomCoatIntersectUnitBox(originUvw, dirUvw, tEnter, tExit))
	{
		return 0.0;
	}
	if (tExit <= tEnter)
	{
		return 0.0;
	}

	// tEnter / tExit are in OBJECT-SPACE metres, because dirUvw was built from
	// a unit object-space direction scaled by invExtent and the box test is in
	// the unit cube. So the span is already the world distance the ray spends
	// inside the coat, and the step compares against it directly.
	float span = tExit - tEnter;
	float steps = ceil(span / stepWorld);
	// Clamped rather than trusted: a tiny stepWorld from a corrupt uniform
	// would otherwise ask for an unbounded march.
	steps = clamp(steps, 1.0, float(OLO_GROOM_COAT_MAX_STEPS));
	float dt = span / steps;
	int stepCount = int(steps);

	float tau = 0.0;
	for (int i = 0; i < OLO_GROOM_COAT_MAX_STEPS; ++i)
	{
		if (i >= stepCount)
		{
			break;
		}
		// Sampled at the MIDPOINT of each step, matching the CPU march, so the
		// two integrate the same Riemann sum rather than two that differ by
		// half a step at each end.
		float t = tEnter + dt * (float(i) + 0.5);
		vec3 uvw = originUvw + dirUvw * t;

		OloGroomCoatVolumeSample sampled = oloGroomCoatSampleVolume(coatVolume, uvw);
		if (sampled.Density <= 0.0)
		{
			continue;
		}

		float meanSine = oloGroomCoatMeanSine(sampled.Direction, dirObject, mode);
		tau += sampled.Density * meanSine * dt;
	}

	return tau;
}

// exp(-tau * (1 - exp(-kappa))), clamped. THE TWIN OF
// GroomCoatShadow::CoatTransmittance — the two must stay the same expression.
//
// THE MEAN OF THE TRANSMITTANCES, NOT THE TRANSMITTANCE OF THE MEAN (#1360).
// `opticalDepth` is E[N], the expected fibre crossings over the footprint, and
// what the footprint receives is E[exp(-kappa N)]. Jensen puts the naive
// exp(-kappa * tau) strictly below that, so it over-darkens a disordered coat.
// For a Poisson N the exact mean is that distribution's probability generating
// function at exp(-kappa), which is the expression above — one extra exp on a
// value already being exponentiated.
//
// kappa is the per-crossing extinction and is DIMENSIONLESS: it describes ONE
// FIBRE, so 1.0 means a single crossing passes 1/e of the light through it. It
// is an authored coat property, deliberately separate from the pigment, which
// already attenuates INSIDE the fibre in GroomFibreCommon.glsl.
float oloGroomCoatTransmittance(float opticalDepth, float kappa)
{
	if (!(opticalDepth > 0.0) || !(kappa > 0.0) || isinf(opticalDepth) || isinf(kappa))
	{
		// Also catches NaN, because a NaN fails every comparison: an unshadowed
		// coat is the loud failure, a black one is the silent one.
		//
		// AN INFINITY IS THE SAME LOUD FAILURE, and it needs its own test
		// because it is the one non-finite value that passes `> 0.0`. The march
		// cannot produce one from a finite density over a finite span, so an
		// infinite tau means the volume or its uniforms are corrupt — and
		// exp(-inf) would quietly return a BLACK coat, which is indistinguishable
		// from a correct silhouette. The CPU twin has always rejected it; this
		// side did not, which made the pair disagree on exactly the input that
		// matters most.
		return 1.0;
	}
	// The CPU twin uses expm1 here, which keeps a small kappa's significant
	// digits; GLSL has no expm1 and the difference is below the f32 epsilon of
	// the exp() it feeds.
	float perCrossing = 1.0 - exp(-kappa);
	return clamp(exp(-perCrossing * opticalDepth), 0.0, 1.0);
}

#endif // OLO_GROOM_COAT_SHADOW_COMMON_GLSL

// GroomAmbient.glsl — the scene's ambient for a fibre, issue #1618.
//
// THE BODY'S RUNG, SAMPLED THE FIBRE'S WAY. The fibre's ambient response wants
// the average radiance along two directions -- the eye-facing normal, and its
// opposite for the paths that look behind the fibre -- from the same source the
// body's ambient ladder takes (include/AmbientLadder.glsl,
// oloSelectAmbientDiffuseRung): the probe volume (realtime DDGI, baked SH, or
// the hybrid blend of the two, as sampleProbeVolumeIrradiance blends them),
// then the sky's irradiance cube, then the flat fill.
//
// WHY NOT CALL THE LADDER. sampleLightProbeGrid blends nine vec3 coefficients
// from each of eight probes into an array and evaluates it, once per
// direction. In the strand shader -- the fibre's quadrature loops already hold
// most of the registers -- that array cost a third of the occupancy even with
// no probe volume bound: GroomPass went from 3.2 to 6.2 ms with one light and
// to 15 ms with 256 (GroomSceneLightingEvidenceTest's cost case). The SH
// evaluation is LINEAR in the coefficients, so each probe's contribution is
// accumulated directly, with no array, and the two directions come from one
// pass over the grid: band 1 is odd in the direction, bands 0 and 2 even, so
// E(-n) is the even sum minus the odd one. The clamp to zero is taken after
// the blend, where evaluateSHCosineIrradiance takes it. GroomAmbientShaderTest
// holds this to sampleLightProbeGrid, direction by direction.
//
// Include after PBRCommon.glsl (oloNormalizedIrradiance), LightProbeSampling
// .glsl (the probe volume) and AmbientLadder.glsl (OLO_AMBIENT_FLAT_FILL).

#ifndef GROOM_AMBIENT_GLSL
#define GROOM_AMBIENT_GLSL

// sampleLightProbeGrid's irradiance E along `n` (front) and `-n` (back).
void oloGroomProbeGridPair(vec3 worldPos, vec3 n, out vec3 front, out vec3 back)
{
	front = vec3(0.0);
	back = vec3(0.0);
	if (u_ProbesEnabled == 0 || !isInsideProbeVolume(worldPos))
	{
		return;
	}
	vec3 gridPos = worldToProbeGrid(worldPos);
	ivec3 p0 = ivec3(floor(gridPos));
	ivec3 p1 = min(p0 + ivec3(1), u_ProbeGridDimensions.xyz - ivec3(1));
	p0 = max(p0, ivec3(0));
	vec3 frac = gridPos - vec3(p0);

	// evaluateSHCosineIrradiance's per-band weights folded into the basis.
	const float A0 = 3.14159265359;
	const float A1 = 2.09439510239;
	const float A2 = 0.78539816340;
	float w0 = A0 * SH_Y00;
	vec3 w1 = A1 * vec3(SH_Y1n1 * n.y, SH_Y10 * n.z, SH_Y11 * n.x);
	float w4 = A2 * SH_Y2n2 * n.x * n.y;
	float w5 = A2 * SH_Y2n1 * n.y * n.z;
	float w6 = A2 * SH_Y20 * (3.0 * n.z * n.z - 1.0);
	float w7 = A2 * SH_Y21 * n.x * n.z;
	float w8 = A2 * SH_Y22 * (n.x * n.x - n.y * n.y);

	vec3 even = vec3(0.0);
	vec3 odd = vec3(0.0);
	float totalWeight = 0.0;
	for (int dz = 0; dz <= 1; ++dz)
	{
		for (int dy = 0; dy <= 1; ++dy)
		{
			for (int dx = 0; dx <= 1; ++dx)
			{
				ivec3 corner = ivec3(dx == 0 ? p0.x : p1.x, dy == 0 ? p0.y : p1.y, dz == 0 ? p0.z : p1.z);
				int base = probeLinearIndex(corner.x, corner.y, corner.z) * SH_COEFFICIENT_COUNT;
				vec4 c0 = u_ProbeSHCoeffs[base];
				if (c0.w < 0.5)
				{
					continue; // invalid probe (inside geometry)
				}
				float wx = dx == 0 ? (1.0 - frac.x) : frac.x;
				float wy = dy == 0 ? (1.0 - frac.y) : frac.y;
				float wz = dz == 0 ? (1.0 - frac.z) : frac.z;
				float w = wx * wy * wz;
				even += w * (c0.xyz * w0 + u_ProbeSHCoeffs[base + 4].xyz * w4 + u_ProbeSHCoeffs[base + 5].xyz * w5 +
				             u_ProbeSHCoeffs[base + 6].xyz * w6 + u_ProbeSHCoeffs[base + 7].xyz * w7 +
				             u_ProbeSHCoeffs[base + 8].xyz * w8);
				odd += w * (u_ProbeSHCoeffs[base + 1].xyz * w1.x + u_ProbeSHCoeffs[base + 2].xyz * w1.y +
				            u_ProbeSHCoeffs[base + 3].xyz * w1.z);
				totalWeight += w;
			}
		}
	}
	if (totalWeight < 0.001)
	{
		return;
	}
	even /= totalWeight;
	odd /= totalWeight;
	front = max(even + odd, vec3(0.0)) * u_ProbeIntensity;
	back = max(even - odd, vec3(0.0)) * u_ProbeIntensity;
}

// One direction's rung below the probe volume: the sky's cube at its IBL
// intensity, or the flat fill (oloSelectAmbientDiffuseRung's last two rungs).
vec3 oloGroomAmbientFallback(samplerCube irradianceMap, vec3 direction, bool enableIBL, float iblIntensity)
{
	return enableIBL ? texture(irradianceMap, direction).rgb * iblIntensity : OLO_AMBIENT_FLAT_FILL;
}

// The average radiance along `n` (front) and `-n` (behind) at `worldPos`, from
// the rung the body's ladder picks for each -- normalised irradiance E/pi times
// the rung's scale, the IBL intensity on every rung while IBL is on.
void oloGroomAmbientPair(vec3 worldPos, vec3 n, vec3 V, samplerCube irradianceMap, bool enableIBL, bool enableProbes,
                         float iblIntensity, out vec3 front, out vec3 behind)
{
	float scale = enableIBL ? iblIntensity : 1.0;
	vec3 probeFront = vec3(0.0);
	vec3 probeBehind = vec3(0.0);
	if (enableProbes)
	{
		// sampleProbeVolumeIrradiance, for both directions.
		if ((u_DDGIEnabled == 0) || (u_DDGIHybridBlend < 0.999))
		{
			oloGroomProbeGridPair(worldPos, n, probeFront, probeBehind);
		}
		if (u_DDGIEnabled != 0)
		{
			vec3 ddgiFront = sampleDDGIIrradiance(worldPos, n, V);
			vec3 ddgiBehind = sampleDDGIIrradiance(worldPos, -n, V);
			probeFront = dot(ddgiFront, ddgiFront) > 0.0 ? mix(probeFront, ddgiFront, u_DDGIHybridBlend) : probeFront;
			probeBehind = dot(ddgiBehind, ddgiBehind) > 0.0 ? mix(probeBehind, ddgiBehind, u_DDGIHybridBlend) : probeBehind;
		}
	}
	front = dot(probeFront, probeFront) > 0.0 ? oloNormalizedIrradiance(probeFront) * scale
	                                          : oloGroomAmbientFallback(irradianceMap, n, enableIBL, iblIntensity);
	behind = dot(probeBehind, probeBehind) > 0.0 ? oloNormalizedIrradiance(probeBehind) * scale
	                                             : oloGroomAmbientFallback(irradianceMap, -n, enableIBL, iblIntensity);
}

#endif // GROOM_AMBIENT_GLSL

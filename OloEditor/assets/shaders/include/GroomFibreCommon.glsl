// GroomFibreCommon.glsl — the fibre BCSDF, issue #1247.
//
// EVERYTHING IN THIS FILE HAS A C++ TWIN in OloEngine/Groom/GroomFibreScattering.h
// and .cpp, and GroomFibreGpuParityTest renders this arithmetic over a grid of
// angles and compares every texel against the C++ side. Same contract as
// #1246's GroomStrandCommon.glsl, and for the same reason: the lobe comparison
// in docs/analysis/groom-fibre-scattering-1247.md is computed by the C++ side,
// so a shader that scattered differently would make every measured number a
// measurement of something that is not on screen.
//
// Unlike #1246's hash this is floating point, so the parity test asks for a
// relative tolerance rather than exact equality. The ORDER OF OPERATIONS is
// still mirrored line for line — a mathematically equal rearrangement moves the
// last bits, and a tolerance that has to absorb that stops being able to catch
// a real divergence.
//
// WHAT THE MODEL IS, and why each piece is here, is documented once in
// GroomFibreScattering.h. This file deliberately does not repeat it: two
// copies of a physical explanation drift, and the twin that matters is the
// arithmetic.
//
// THE ONE THING WORTH KNOWING HERE. The longitudinal term M does not depend on
// h, so it is evaluated ONCE PER LOBE and the quadrature loop carries only the
// attenuation and the azimuthal term. Inside the loop it would cost N Bessel
// evaluations per lobe, which is the difference between a four-tap rule being
// affordable in a fragment shader and not.

#ifndef GROOM_FIBRE_COMMON_GLSL
#define GROOM_FIBRE_COMMON_GLSL

#define OLO_GROOM_FIBRE_LOBES 4

// The h-quadrature loop's COMPILE-TIME bound. The live order arrives as a
// uniform and the loop breaks at it: a uniform-driven bound cannot be unrolled,
// which is the same reason LightSampling.glsl bounds its own loops with a
// constant (see glsl-shaders.md §8b on data-dependent loops).
#define OLO_GROOM_FIBRE_MAX_H 32

// Twin of GroomFibreDebugMode.
#define OLO_GROOM_FIBRE_DEBUG_FULL 0
#define OLO_GROOM_FIBRE_DEBUG_R 1
#define OLO_GROOM_FIBRE_DEBUG_TT 2
#define OLO_GROOM_FIBRE_DEBUG_TRT 3
#define OLO_GROOM_FIBRE_DEBUG_RESIDUAL 4
#define OLO_GROOM_FIBRE_DEBUG_TANGENT 5

#define OLO_GROOM_FIBRE_PI 3.14159265358979323846
#define OLO_GROOM_FIBRE_TWO_PI 6.28318530717958647692
#define OLO_GROOM_FIBRE_INV_TWO_PI 0.15915494309189533577

// Twin of GroomFibreParams. Filled from the strand pass's UBO by
// oloGroomFibreFromUniforms so the unpacking lives in one place.
struct OloGroomFibre
{
	vec3 SigmaA;
	float Eta;
	// V[0] only: V[1] = V[0]/4, V[2] = 4 V[0] and V[3] = V[2] are EXACT in
	// binary floating point, so deriving them here cannot disagree with the
	// C++ side. Uploading four lanes that are three multiplications apart
	// would be four chances for them to.
	float V0;
	float S;
	float Intensity;
	vec3 Sin2kAlpha;
	vec3 Cos2kAlpha;
	int HSamples;
};

// The four lobes, kept apart so a debug mode can render one. Twin of
// GroomFibreLobeSet.
struct OloGroomFibreLobes
{
	vec3 R;
	vec3 TT;
	vec3 TRT;
	vec3 Residual;
};

vec3 oloGroomFibreSum(OloGroomFibreLobes lobes)
{
	return lobes.R + lobes.TT + lobes.TRT + lobes.Residual;
}

vec3 oloGroomFibreSelect(OloGroomFibreLobes lobes, int debugMode)
{
	if (debugMode == OLO_GROOM_FIBRE_DEBUG_R)
		return lobes.R;
	if (debugMode == OLO_GROOM_FIBRE_DEBUG_TT)
		return lobes.TT;
	if (debugMode == OLO_GROOM_FIBRE_DEBUG_TRT)
		return lobes.TRT;
	if (debugMode == OLO_GROOM_FIBRE_DEBUG_RESIDUAL)
		return lobes.Residual;
	return oloGroomFibreSum(lobes);
}

float oloGroomFibreSqr(float x)
{
	return x * x;
}

float oloGroomFibreSafeSqrt(float x)
{
	return sqrt(max(0.0, x));
}

float oloGroomFibreSafeASin(float x)
{
	return asin(clamp(x, -1.0, 1.0));
}

// 1 / (4^i (i!)^2), i = 0..9 — the series coefficients of the modified Bessel
// function of the first kind, order zero. Written out because GLSL has neither
// a factorial nor a 64-bit integer to build them with, and accumulated in this
// order because the C++ twin accumulates in this order.
const float kOloGroomI0[14] = float[14](1.0, 2.5e-1, 1.5625e-2, 4.34027778e-4, 6.78168403e-6, 6.78168403e-8,
                                        4.70950280e-10, 2.40280755e-12, 9.38596699e-15, 2.89690339e-17,
                                        7.24225848e-20, 1.49633440e-22, 2.59780277e-25, 3.84290351e-28);

float oloGroomBesselI0(float x)
{
	float x2 = x * x;
	float x2i = 1.0;
	float value = 0.0;
	for (int i = 0; i < 14; ++i)
	{
		value += x2i * kOloGroomI0[i];
		x2i *= x2;
	}
	return value;
}

// Past the crossover the series stops converging usefully, so the asymptotic
// expansion takes over. Both halves differ from pbrt-v3's — the 1/(8x) term is
// NOT inside the 0.5 factor, and the series runs to fourteen terms — which
// takes the discontinuity at x = 12 from 1.50 % to 0.0002 %. The measurement
// and the reason it matters are in GroomFibreScattering.cpp's twin.
float oloGroomLogBesselI0(float x)
{
	if (x > 12.0)
	{
		return x - (0.5 * log(OLO_GROOM_FIBRE_TWO_PI)) - (0.5 * log(x)) +
		       log(1.0 + (0.125 / x) + (9.0 / (128.0 * x * x)));
	}
	return log(oloGroomBesselI0(x));
}

// The longitudinal scattering function. The v <= 0.1 branch is not an
// optimisation: at small variance the 1/sinh(1/v) normalisation underflows
// while I0(a) overflows, so the ratio is taken in log space instead.
float oloGroomLongitudinalM(float cosThetaI, float cosThetaO, float sinThetaI, float sinThetaO, float v)
{
	float a = (cosThetaI * cosThetaO) / v;
	float b = (sinThetaI * sinThetaO) / v;
	if (v <= 0.1)
	{
		return exp(oloGroomLogBesselI0(a) - b - (1.0 / v) + 0.6931 + log(1.0 / (2.0 * v)));
	}
	return (exp(-b) * oloGroomBesselI0(a)) / (sinh(1.0 / v) * 2.0 * v);
}

float oloGroomLogistic(float x, float s)
{
	float e = exp(-abs(x) / s);
	return e / (s * oloGroomFibreSqr(1.0 + e));
}

float oloGroomLogisticCDF(float x, float s)
{
	return 1.0 / (1.0 + exp(-x / s));
}

float oloGroomTrimmedLogistic(float x, float s)
{
	float norm = oloGroomLogisticCDF(OLO_GROOM_FIBRE_PI, s) - oloGroomLogisticCDF(-OLO_GROOM_FIBRE_PI, s);
	return oloGroomLogistic(x, s) / norm;
}

float oloGroomLobePhi(int p, float gammaO, float gammaT)
{
	float pf = float(p);
	return (2.0 * pf * gammaT) - (2.0 * gammaO) + (pf * OLO_GROOM_FIBRE_PI);
}

// Wrap to [-pi, pi) with no loop. The obvious `while (d > pi) d -= 2pi` form is
// a data-dependent loop, which glsl-shaders.md §8b names as a compile-time bomb
// on Mesa's AMD path.
float oloGroomWrapPhi(float phi)
{
	return phi - (OLO_GROOM_FIBRE_TWO_PI * floor((phi + OLO_GROOM_FIBRE_PI) * OLO_GROOM_FIBRE_INV_TWO_PI));
}

float oloGroomAzimuthalN(float phi, int p, float s, float gammaO, float gammaT)
{
	return oloGroomTrimmedLogistic(oloGroomWrapPhi(phi - oloGroomLobePhi(p, gammaO, gammaT)), s);
}

// The quadrature's roughness floor. Widening each node's azimuthal lobe to
// cover its own slice of h is what turns the rule from N spikes into a
// reconstruction of the far field; the full argument, and the measurement that
// rejected the unwidened rule, are in GroomFibreScattering.cpp's
// NodeWidenedScale and in the analysis.
float oloGroomNodeWidenedScale(float s, int p, float h, float etaPrime, float nodeWidth)
{
	float safeEta = max(etaPrime, 1.0e-3);
	float dGammaO = 1.0 / max(oloGroomFibreSafeSqrt(1.0 - oloGroomFibreSqr(h)), 1.0e-3);
	float dGammaT = (1.0 / safeEta) / max(oloGroomFibreSafeSqrt(1.0 - oloGroomFibreSqr(h / safeEta)), 1.0e-3);
	float dPhi = abs((2.0 * float(p) * dGammaT) - (2.0 * dGammaO));
	return max(s, min(0.25 * dPhi * nodeWidth, OLO_GROOM_FIBRE_PI));
}

float oloGroomFresnelDielectric(float cosThetaI, float eta)
{
	cosThetaI = clamp(cosThetaI, -1.0, 1.0);
	float etaI = 1.0;
	float etaT = eta;
	if (cosThetaI <= 0.0)
	{
		etaI = eta;
		etaT = 1.0;
		cosThetaI = -cosThetaI;
	}

	float sinThetaI = oloGroomFibreSafeSqrt(1.0 - oloGroomFibreSqr(cosThetaI));
	float sinThetaT = (etaI / etaT) * sinThetaI;
	if (sinThetaT >= 1.0)
	{
		return 1.0; // total internal reflection
	}
	float cosThetaT = oloGroomFibreSafeSqrt(1.0 - oloGroomFibreSqr(sinThetaT));

	float rParl = ((etaT * cosThetaI) - (etaI * cosThetaT)) / ((etaT * cosThetaI) + (etaI * cosThetaT));
	float rPerp = ((etaI * cosThetaI) - (etaT * cosThetaT)) / ((etaI * cosThetaI) + (etaT * cosThetaT));
	return ((rParl * rParl) + (rPerp * rPerp)) * 0.5;
}

// The four path attenuations. They sum to exactly one when the fibre absorbs
// nothing, whatever the Fresnel term is, which is the model's energy
// conservation — see GroomFibreScattering.cpp's Attenuations for the identity.
//
// Returned as a named struct rather than through an `out vec3 ap[4]`, and
// nothing in this file indexes an array by a loop variable. That is a
// deliberate restriction rather than a workaround for a known defect:
// glsl-shaders.md §8b warns that a data-dependent loop over indexed arrays is
// a compile-time bomb on Mesa's AMD path, and this file goes through
// GLSL -> SPIR-V -> SPIRV-Cross -> GLSL on the GL route, which is a long way
// for an array to travel for no benefit. Four lobes are four named values.
OloGroomFibreLobes oloGroomAttenuations(float cosThetaO, float eta, float h, vec3 transmittance)
{
	float cosGammaO = oloGroomFibreSafeSqrt(1.0 - oloGroomFibreSqr(h));
	float f = oloGroomFresnelDielectric(cosThetaO * cosGammaO, eta);

	OloGroomFibreLobes ap;
	ap.R = vec3(f);
	ap.TT = oloGroomFibreSqr(1.0 - f) * transmittance;
	ap.TRT = ap.TT * transmittance * f;

	vec3 denom = max(vec3(1.0) - (transmittance * f), vec3(1.0e-5));
	ap.Residual = (ap.TRT * transmittance * f) / denom;
	return ap;
}

// The per-lobe cuticle tilt, applied as a rotation of the outgoing
// longitudinal angle. Twin of ApplyTilt.
// RETURNS a vec2 (sin, cos) rather than writing two `out` floats, so four
// sequential calls need four independent results rather than one shared pair of
// output locals. The C++ twin uses out-references, which read more naturally
// there; here a returned value leaves nothing to alias.
vec2 oloGroomApplyTilt(OloGroomFibre fibre, int p, float sinThetaO, float cosThetaO)
{
	vec2 result;
	if (p == 0)
	{
		result.x = (sinThetaO * fibre.Cos2kAlpha.y) - (cosThetaO * fibre.Sin2kAlpha.y);
		result.y = (cosThetaO * fibre.Cos2kAlpha.y) + (sinThetaO * fibre.Sin2kAlpha.y);
	}
	else if (p == 1)
	{
		result.x = (sinThetaO * fibre.Cos2kAlpha.x) + (cosThetaO * fibre.Sin2kAlpha.x);
		result.y = (cosThetaO * fibre.Cos2kAlpha.x) - (sinThetaO * fibre.Sin2kAlpha.x);
	}
	else if (p == 2)
	{
		result.x = (sinThetaO * fibre.Cos2kAlpha.z) + (cosThetaO * fibre.Sin2kAlpha.z);
		result.y = (cosThetaO * fibre.Cos2kAlpha.z) - (sinThetaO * fibre.Sin2kAlpha.z);
	}
	else
	{
		result.x = sinThetaO;
		result.y = cosThetaO;
	}
	result.y = abs(result.y);
	return result;
}

// The FAR-FIELD lobes. `sinThetaO` and `sinThetaI` are the components of the
// two directions along the strand tangent; `phi` is the azimuth DIFFERENCE
// about that tangent, in [0, pi].
OloGroomFibreLobes oloGroomFibreEvaluate(OloGroomFibre fibre, float sinThetaO, float sinThetaI, float phi)
{
	int n = clamp(fibre.HSamples, 1, OLO_GROOM_FIBRE_MAX_H);
	float nodeWidth = 2.0 / float(n);

	sinThetaO = clamp(sinThetaO, -1.0, 1.0);
	sinThetaI = clamp(sinThetaI, -1.0, 1.0);
	float cosThetaO = oloGroomFibreSafeSqrt(1.0 - oloGroomFibreSqr(sinThetaO));
	float cosThetaI = oloGroomFibreSafeSqrt(1.0 - oloGroomFibreSqr(sinThetaI));

	// Hoisted: neither M nor etaPrime depends on h. Unrolled per lobe rather
	// than looped over an array — see the note above oloGroomAttenuations.
	vec2 tiltR = oloGroomApplyTilt(fibre, 0, sinThetaO, cosThetaO);
	vec2 tiltTT = oloGroomApplyTilt(fibre, 1, sinThetaO, cosThetaO);
	vec2 tiltTRT = oloGroomApplyTilt(fibre, 2, sinThetaO, cosThetaO);
	vec2 tiltResidual = oloGroomApplyTilt(fibre, 3, sinThetaO, cosThetaO);
	float longitudinalR = oloGroomLongitudinalM(cosThetaI, tiltR.y, sinThetaI, tiltR.x, fibre.V0);
	float longitudinalTT = oloGroomLongitudinalM(cosThetaI, tiltTT.y, sinThetaI, tiltTT.x, 0.25 * fibre.V0);
	float longitudinalTRT = oloGroomLongitudinalM(cosThetaI, tiltTRT.y, sinThetaI, tiltTRT.x, 4.0 * fibre.V0);
	float longitudinalResidual =
	    oloGroomLongitudinalM(cosThetaI, tiltResidual.y, sinThetaI, tiltResidual.x, 4.0 * fibre.V0);

	float etaPrime =
	    oloGroomFibreSafeSqrt(oloGroomFibreSqr(fibre.Eta) - oloGroomFibreSqr(sinThetaO)) / max(cosThetaO, 1.0e-5);
	float sinThetaT = sinThetaO / fibre.Eta;
	float cosThetaT = oloGroomFibreSafeSqrt(1.0 - oloGroomFibreSqr(sinThetaT));

	vec3 azimuthalR = vec3(0.0);
	vec3 azimuthalTT = vec3(0.0);
	vec3 azimuthalTRT = vec3(0.0);
	vec3 azimuthalResidual = vec3(0.0);

	for (int k = 0; k < OLO_GROOM_FIBRE_MAX_H; ++k)
	{
		if (k >= n)
		{
			break;
		}
		float h = -1.0 + ((2.0 * (float(k) + 0.5)) / float(n));
		float gammaO = oloGroomFibreSafeASin(h);
		float sinGammaT = clamp(h / etaPrime, -1.0, 1.0);
		float cosGammaT = oloGroomFibreSafeSqrt(1.0 - oloGroomFibreSqr(sinGammaT));
		float gammaT = oloGroomFibreSafeASin(sinGammaT);

		vec3 transmittance = exp(-fibre.SigmaA * ((2.0 * cosGammaT) / max(cosThetaT, 1.0e-5)));

		// The attenuations, inline. Six lines of trivial arithmetic in the one
		// loop that uses them, rather than a call whose four results have to be
		// carried back out — see the note above oloGroomAttenuations.
		float cosGammaO = oloGroomFibreSafeSqrt(1.0 - oloGroomFibreSqr(h));
		float fresnel = oloGroomFresnelDielectric(cosThetaO * cosGammaO, fibre.Eta);
		vec3 apR = vec3(fresnel);
		vec3 apTT = oloGroomFibreSqr(1.0 - fresnel) * transmittance;
		vec3 apTRT = apTT * transmittance * fresnel;
		vec3 apResidual = (apTRT * transmittance * fresnel) /
		                  max(vec3(1.0) - (transmittance * fresnel), vec3(1.0e-5));

		azimuthalR += apR * oloGroomAzimuthalN(phi, 0, oloGroomNodeWidenedScale(fibre.S, 0, h, etaPrime, nodeWidth),
		                                       gammaO, gammaT);
		azimuthalTT += apTT * oloGroomAzimuthalN(phi, 1, oloGroomNodeWidenedScale(fibre.S, 1, h, etaPrime, nodeWidth),
		                                         gammaO, gammaT);
		azimuthalTRT +=
		    apTRT * oloGroomAzimuthalN(phi, 2, oloGroomNodeWidenedScale(fibre.S, 2, h, etaPrime, nodeWidth), gammaO,
		                               gammaT);
		// The residual is isotropic in azimuth: it is every path with three or
		// more internal bounces, and those have no shared exit direction left.
		azimuthalResidual += apResidual * OLO_GROOM_FIBRE_INV_TWO_PI;
	}

	float weight = 1.0 / float(n);

	OloGroomFibreLobes lobes;
	lobes.R = azimuthalR * (longitudinalR * weight);
	lobes.TT = azimuthalTT * (longitudinalTT * weight);
	lobes.TRT = azimuthalTRT * (longitudinalTRT * weight);
	lobes.Residual = azimuthalResidual * (longitudinalResidual * weight);
	return lobes;
}

// The renderer's form: a unit strand tangent and two unit world directions,
// `wo` towards the eye and `wi` towards the light.
//
// ONLY THE AZIMUTH DIFFERENCE IS NEEDED, which is what frees the model from
// wanting a binormal — a screen-facing ribbon has no meaningful one, so a model
// that needed it would have to invent one per frame and it would swim as the
// camera turned.
OloGroomFibreLobes oloGroomFibreEvaluateDirections(OloGroomFibre fibre, vec3 tangent, vec3 wo, vec3 wi)
{
	float sinThetaO = clamp(dot(tangent, wo), -1.0, 1.0);
	float sinThetaI = clamp(dot(tangent, wi), -1.0, 1.0);

	vec3 perpO = wo - (tangent * sinThetaO);
	vec3 perpI = wi - (tangent * sinThetaI);
	float lenO = length(perpO);
	float lenI = length(perpI);

	// A direction exactly along the fibre has no azimuth at all. phi = 0 is the
	// continuous limit and keeps a NaN out of the frame; the longitudinal term
	// is already near zero there, so the choice is unobservable.
	float phi = 0.0;
	if (lenO > 1.0e-6 && lenI > 1.0e-6)
	{
		phi = acos(clamp(dot(perpO, perpI) / (lenO * lenI), -1.0, 1.0));
	}

	return oloGroomFibreEvaluate(fibre, sinThetaO, sinThetaI, phi);
}

// THE ENVIRONMENT TERM, per lobe: the fibre's response to a unit uniform
// environment. The caller multiplies by the environment's average radiance
// (an irradiance sample over pi).
//
// It is each path's mean attenuation over the fibre's width — that path's
// albedo — times cos(theta_o), and it costs only the attenuations, which the
// evaluation already computes. The full argument, the measured accuracy band
// and the rejected alternative are in GroomFibreScattering.h.
OloGroomFibreLobes oloGroomFibreAmbientResponse(OloGroomFibre fibre, float sinThetaO)
{
	int n = clamp(fibre.HSamples, 1, OLO_GROOM_FIBRE_MAX_H);
	sinThetaO = clamp(sinThetaO, -1.0, 1.0);

	float cosThetaO = oloGroomFibreSafeSqrt(1.0 - oloGroomFibreSqr(sinThetaO));
	float etaPrime =
	    oloGroomFibreSafeSqrt(oloGroomFibreSqr(fibre.Eta) - oloGroomFibreSqr(sinThetaO)) / max(cosThetaO, 1.0e-5);
	float sinThetaT = sinThetaO / fibre.Eta;
	float cosThetaT = oloGroomFibreSafeSqrt(1.0 - oloGroomFibreSqr(sinThetaT));

	// Named accumulators rather than an array, as in oloGroomFibreEvaluate.
	vec3 totalR = vec3(0.0);
	vec3 totalTT = vec3(0.0);
	vec3 totalTRT = vec3(0.0);
	vec3 totalResidual = vec3(0.0);

	for (int k = 0; k < OLO_GROOM_FIBRE_MAX_H; ++k)
	{
		if (k >= n)
		{
			break;
		}
		float h = -1.0 + ((2.0 * (float(k) + 0.5)) / float(n));
		float sinGammaT = clamp(h / etaPrime, -1.0, 1.0);
		float cosGammaT = oloGroomFibreSafeSqrt(1.0 - oloGroomFibreSqr(sinGammaT));
		vec3 transmittance = exp(-fibre.SigmaA * ((2.0 * cosGammaT) / max(cosThetaT, 1.0e-5)));

		// Inline, as in oloGroomFibreEvaluate.
		float cosGammaO = oloGroomFibreSafeSqrt(1.0 - oloGroomFibreSqr(h));
		float fresnel = oloGroomFresnelDielectric(cosThetaO * cosGammaO, fibre.Eta);
		vec3 apR = vec3(fresnel);
		vec3 apTT = oloGroomFibreSqr(1.0 - fresnel) * transmittance;
		vec3 apTRT = apTT * transmittance * fresnel;
		vec3 apResidual = (apTRT * transmittance * fresnel) /
		                  max(vec3(1.0) - (transmittance * fresnel), vec3(1.0e-5));

		totalR += apR;
		totalTT += apTT;
		totalTRT += apTRT;
		totalResidual += apResidual;
	}

	float weight = cosThetaO / float(n);

	OloGroomFibreLobes lobes;
	lobes.R = totalR * weight;
	lobes.TT = totalTT * weight;
	lobes.TRT = totalTRT * weight;
	lobes.Residual = totalResidual * weight;
	return lobes;
}

// The fibre's projected width as seen from `wi`: a strand lit end-on
// intercepts almost no light per unit length. THIS IS NOT THE SURFACE COSINE,
// and it is not inside the BCSDF either — the BCSDF is normalised over the
// sphere, and this factor belongs to the geometry. Getting it wrong is
// invisible at normal incidence and wrong everywhere else.
float oloGroomFibreCosineWeight(vec3 tangent, vec3 wi)
{
	float sinTheta = clamp(dot(tangent, wi), -1.0, 1.0);
	return oloGroomFibreSafeSqrt(1.0 - oloGroomFibreSqr(sinTheta));
}

#endif // GROOM_FIBRE_COMMON_GLSL
